/**
 * @file src/platform/windows/text_context.cpp
 * @brief Event-driven UI Automation client for the interactive streaming desktop.
 */
#include "text_context.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <uiautomation.h>
#include <windows.h>
#include <wrl/client.h>

namespace text_context::windows {
  namespace {
    using Microsoft::WRL::ComPtr;

    /** @brief Coalesce notifications without retaining provider objects or calling into Sunshine from COM. */
    struct signal_t {
      std::mutex mutex;
      std::condition_variable_any changed;
      bool pending = false;

      void wake() {
        std::lock_guard lock(mutex);
        pending = true;
        changed.notify_one();
      }
    };

    class focus_handler_t final: public IUIAutomationFocusChangedEventHandler {
      std::atomic<ULONG> refs {1};

    public:
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) {
          return E_POINTER;
        }
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IUIAutomationFocusChangedEventHandler)) {
          return E_NOINTERFACE;
        }
        *out = static_cast<IUIAutomationFocusChangedEventHandler *>(this);
        AddRef();
        return S_OK;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --refs;
        if (!left) {
          delete this;
        }
        return left;
      }

      std::shared_ptr<signal_t> signal;

      HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement *) override {
        signal->wake();
        return S_OK;
      }
    };

    /** @brief Accept only a focused, enabled Edit whose provider explicitly reports writable. */
    std::optional<editor_t> inspect(IUIAutomation *automation) {
      // A SYSTEM-hosted stream can switch to Winlogon or UAC. Only query the ordinary user desktop.
      HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
      if (!desktop) {
        return std::nullopt;
      }
      wchar_t name[64] {};
      DWORD size {};
      const bool user_desktop = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) &&
                                CompareStringOrdinal(name, -1, L"Default", -1, TRUE) == CSTR_EQUAL;
      CloseDesktop(desktop);
      if (!user_desktop) {
        return std::nullopt;
      }
      ComPtr<IUIAutomationElement> element;
      CONTROLTYPEID type {};
      BOOL focused {}, enabled {}, readonly {}, password {};
      RECT bounds {};
      ComPtr<IUIAutomationValuePattern> value;
      if (FAILED(automation->GetFocusedElement(&element)) || !element) {
        return std::nullopt;
      }
      if (FAILED(element->get_CurrentControlType(&type)) || type != UIA_EditControlTypeId) {
        return std::nullopt;
      }
      if (FAILED(element->get_CurrentHasKeyboardFocus(&focused)) || !focused || FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled) {
        return std::nullopt;
      }
      if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) || !value || FAILED(value->get_CurrentIsReadOnly(&readonly)) || readonly) {
        return std::nullopt;
      }
      if (FAILED(element->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) || FAILED(element->get_CurrentIsPassword(&password))) {
        return std::nullopt;
      }
      editor_t editor {{bounds.left, bounds.top, bounds.right, bounds.bottom}, {}, password != FALSE};
      // Win32 supplies a physical caret for native editors. Other providers can fall back to the click anchor.
      GUITHREADINFO gui {};
      gui.cbSize = sizeof(gui);
      const auto foreground = GetForegroundWindow();
      if (foreground && GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &gui) && gui.hwndCaret) {
        POINT tl {gui.rcCaret.left, gui.rcCaret.top}, br {gui.rcCaret.right, gui.rcCaret.bottom};
        if (ClientToScreen(gui.hwndCaret, &tl) && ClientToScreen(gui.hwndCaret, &br) && br.y > tl.y && editor.bounds.contains(tl.x, tl.y)) {
          editor.caret = rect_t {tl.x, tl.y, std::max(tl.x + 1, br.x), br.y};
        }
      }
      return editor;
    }

    std::mutex lifecycle;  ///< Serializes first-start and last-stop including worker teardown.
    std::mutex state_mutex;
    router_t router;
    std::shared_ptr<signal_t> signal;
    std::jthread worker;

    /** @brief Run provider calls on a dedicated MTA and only while a remote gesture or focus is relevant. */
    void run(std::stop_token token, std::shared_ptr<signal_t> notifications) {
      if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return;
      }
      {
        ComPtr<IUIAutomation> automation;
        ComPtr<focus_handler_t> handler;
        handler.Attach(new focus_handler_t);
        if (handler && SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
          ComPtr<IUIAutomation2> settings;
          if (SUCCEEDED(automation.As(&settings))) {
            settings->put_ConnectionTimeout(1000);
            settings->put_TransactionTimeout(1000);
          }
          handler->signal = notifications;
          if (SUCCEEDED(automation->AddFocusChangedEventHandler(nullptr, handler.Get()))) {
            while (!token.stop_requested()) {
              {
                std::unique_lock lock(notifications->mutex);
                if (!notifications->changed.wait(lock, token, [&] {
                      return notifications->pending;
                    })) {
                  break;
                }
                notifications->pending = false;
              }
              std::uint64_t generation;
              {
                std::lock_guard lock(state_mutex);
                if (!router.interested(clock_t::now())) {
                  continue;
                }
                generation = router.generation();
              }
              auto editor = inspect(automation.Get());
              std::lock_guard lock(state_mutex);
              router.observe(generation, editor, clock_t::now());
            }
            automation->RemoveFocusChangedEventHandler(handler.Get());
          }
        }
      }
      CoUninitialize();
    }
  }  // namespace

  void start(session_id id) {
    std::lock_guard guard(lifecycle);
    std::lock_guard lock(state_mutex);
    router.subscribe(id);
    if (!worker.joinable()) {
      signal = std::make_shared<signal_t>();
      worker = std::jthread(run, signal);
    } else {
      signal->wake();
    }
  }

  void stop(session_id id) {
    std::lock_guard guard(lifecycle);
    {
      std::lock_guard lock(state_mutex);
      router.unsubscribe(id);
      if (!router.empty()) {
        signal->wake();
        return;
      }
      signal.reset();
    }
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
  }

  void pointer(session_id id, std::uint64_t key, action_e action, int x, int y, rect_t capture) {
    std::lock_guard lock(state_mutex);
    if (router.pointer(id, key, action, x, y, capture, clock_t::now()) && signal) {
      signal->wake();
    }
  }

  std::vector<packet_t> drain(session_id id) {
    std::lock_guard lock(state_mutex);
    return router.drain(id);
  }
}  // namespace text_context::windows
