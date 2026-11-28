/**
 * @file tests/unit/test_windows_text_context.cpp
 * @brief Interactive smoke test using the system UI Automation provider for Win32 Edit.
 */
// test includes
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <atomic>
  #include <cstdlib>
  #include <thread>

  // platform includes
  #include <windows.h>

  // local includes
  #include "src/platform/windows/text_context.h"

namespace {
  /** @brief Pump the provider's window thread while the separate MTA queries UIA. */
  void pump_messages() {
    MSG msg {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }

  TEST(WindowsRemoteTextContext, ReportsWritableEditAndClearsOnReadOnlyFocus) {
    if (!std::getenv("SUNSHINE_TEST_INTERACTIVE_FOCUS")) {
      GTEST_SKIP() << "Set SUNSHINE_TEST_INTERACTIVE_FOCUS=1 on an interactive Windows desktop.";
    }
    const auto previous = GetForegroundWindow();
    const auto window = CreateWindowW(L"STATIC", L"Sunshine focus test", WS_OVERLAPPEDWINDOW, 100, 100, 400, 180, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    auto cleanup = util::fail_guard([&] {
      DestroyWindow(window);
      if (previous) {
        SetForegroundWindow(previous);
      }
    });
    const auto writable = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE, 8, 8, 250, 32, window, nullptr, nullptr, nullptr);
    const auto readonly = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY, 8, 60, 250, 32, window, nullptr, nullptr, nullptr);
    ASSERT_NE(writable, nullptr);
    ASSERT_NE(readonly, nullptr);
    ShowWindow(window, SW_SHOW);
    const auto foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const auto attached = AttachThreadInput(GetCurrentThreadId(), foreground_thread, TRUE);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    if (attached) {
      AttachThreadInput(GetCurrentThreadId(), foreground_thread, FALSE);
    }
    if (GetForegroundWindow() != window) {
      GTEST_SKIP() << "Windows did not grant foreground focus to the test window.";
    }

    constexpr text_context::session_id id = 1;
    text_context::windows::start(id);
    // Provider cleanup can involve this window thread. Keep pumping until teardown finishes.
    auto stop = util::fail_guard([&] {
      std::atomic_bool stopped {};
      std::jthread teardown([&] {
        text_context::windows::stop(id);
        stopped = true;
      });
      while (!stopped) {
        pump_messages();
      }
    });
    auto click = [&](HWND target) {
      SetFocus(target);
      RECT rect {};
      GetWindowRect(target, &rect);
      const text_context::rect_t capture {rect.left, rect.top, rect.right, rect.bottom};
      for (auto action : {text_context::action_e::down, text_context::action_e::up}) {
        text_context::windows::pointer(id, text_context::mouse_pointer, action, rect.left + 4, rect.top + 4, capture);
      }
    };
    auto next = [&]() -> std::optional<text_context::packet_t> {
      const auto until = text_context::clock_t::now() + std::chrono::seconds(4);
      do {
        pump_messages();
        auto packets = text_context::windows::drain(id);
        if (!packets.empty()) {
          return packets.back();
        }
      } while (text_context::clock_t::now() < until);
      return std::nullopt;
    };
    click(writable);
    const auto active = next();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ((*active)[2] & 3, 3);
    click(readonly);
    const auto inactive = next();
    ASSERT_TRUE(inactive.has_value());
    EXPECT_EQ((*inactive)[2] & 3, 0);
    EXPECT_EQ((*active)[8], (*inactive)[8]);
  }
}  // namespace
#endif
