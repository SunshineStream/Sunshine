/**
 * @file src/platform/windows/display_base.cpp
 * @brief todo
 */
#include <cmath>
#include <initguid.h>
#include <thread>

#include <boost/process.hpp>

// We have to include boost/process.hpp before display.h due to WinSock.h,
// but that prevents the definition of NTSTATUS so we must define it ourself.
typedef long NTSTATUS;

#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/stat_trackers.h"
#include "src/video.h"

namespace platf {
  using namespace std::literals;
}
namespace platf::dxgi {
  namespace bp = boost::process;

  capture_e
  duplication_t::next_frame(DXGI_OUTDUPL_FRAME_INFO &frame_info, std::chrono::milliseconds timeout, resource_t::pointer *res_p) {
    auto capture_status = release_frame();
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto status = dup->AcquireNextFrame(timeout.count(), &frame_info, res_p);

    switch (status) {
      case S_OK:
        // ProtectedContentMaskedOut seems to semi-randomly be TRUE or FALSE even when protected content
        // is on screen the whole time, so we can't just print when it changes. Instead we'll keep track
        // of the last time we printed the warning and print another if we haven't printed one recently.
        if (frame_info.ProtectedContentMaskedOut && std::chrono::steady_clock::now() > last_protected_content_warning_time + 10s) {
          BOOST_LOG(warning) << "Windows is currently blocking DRM-protected content from capture. You may see black regions where this content would be."sv;
          last_protected_content_warning_time = std::chrono::steady_clock::now();
        }

        has_frame = true;
        return capture_e::ok;
      case DXGI_ERROR_WAIT_TIMEOUT:
        return capture_e::timeout;
      case WAIT_ABANDONED:
      case DXGI_ERROR_ACCESS_LOST:
      case DXGI_ERROR_ACCESS_DENIED:
        return capture_e::reinit;
      default:
        BOOST_LOG(error) << "Couldn't acquire next frame [0x"sv << util::hex(status).to_string_view();
        return capture_e::error;
    }
  }

  capture_e
  duplication_t::reset(dup_t::pointer dup_p) {
    auto capture_status = release_frame();

    dup.reset(dup_p);

    return capture_status;
  }

  capture_e
  duplication_t::release_frame() {
    if (!has_frame) {
      return capture_e::ok;
    }

    auto status = dup->ReleaseFrame();
    has_frame = false;
    switch (status) {
      case S_OK:
        return capture_e::ok;

      case DXGI_ERROR_INVALID_CALL:
        BOOST_LOG(warning) << "Duplication frame already released";
        return capture_e::ok;

      case DXGI_ERROR_ACCESS_LOST:
        return capture_e::reinit;

      default:
        BOOST_LOG(error) << "Error while releasing duplication frame [0x"sv << util::hex(status).to_string_view();
        return capture_e::error;
    }
  }

  duplication_t::~duplication_t() {
    release_frame();
  }

  void
  display_base_t::high_precision_sleep(std::chrono::nanoseconds duration) {
    if (!timer) {
      BOOST_LOG(error) << "Attempting high_precision_sleep() with uninitialized timer";
      return;
    }
    if (duration < 0s) {
      BOOST_LOG(error) << "Attempting high_precision_sleep() with negative duration";
      return;
    }
    if (duration > 5s) {
      BOOST_LOG(error) << "Attempting high_precision_sleep() with unexpectedly large duration (>5s)";
      return;
    }

    LARGE_INTEGER due_time;
    due_time.QuadPart = duration.count() / -100;
    SetWaitableTimer(timer.get(), &due_time, 0, nullptr, nullptr, false);
    WaitForSingleObject(timer.get(), INFINITE);
  }

  capture_e
  display_base_t::capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) {
    auto adjust_client_frame_rate = [&]() -> DXGI_RATIONAL {
      // Adjust capture frame interval when display refresh rate is not integral but very close to requested fps.
      if (display_refresh_rate.Denominator > 1) {
        DXGI_RATIONAL candidate = display_refresh_rate;
        if (client_frame_rate % display_refresh_rate_rounded == 0) {
          candidate.Numerator *= client_frame_rate / display_refresh_rate_rounded;
        }
        else if (display_refresh_rate_rounded % client_frame_rate == 0) {
          candidate.Denominator *= display_refresh_rate_rounded / client_frame_rate;
        }
        double candidate_rate = (double) candidate.Numerator / candidate.Denominator;
        // Can only decrease requested fps, otherwise client may start accumulating frames and suffer increased latency.
        if (client_frame_rate > candidate_rate && candidate_rate / client_frame_rate > 0.99) {
          BOOST_LOG(info) << "Adjusted capture rate to " << candidate_rate << "fps to better match display";
          return candidate;
        }
      }

      return { (uint32_t) client_frame_rate, 1 };
    };

    DXGI_RATIONAL client_frame_rate_adjusted = adjust_client_frame_rate();
    std::optional<std::chrono::steady_clock::time_point> frame_pacing_group_start;
    uint32_t frame_pacing_group_frames = 0;

    // Keep the display awake during capture. If the display goes to sleep during
    // capture, best case is that capture stops until it powers back on. However,
    // worst case it will trigger us to reinit DD, waking the display back up in
    // a neverending cycle of waking and sleeping the display of an idle machine.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    auto clear_display_required = util::fail_guard([]() {
      SetThreadExecutionState(ES_CONTINUOUS);
    });

    sleep_overshoot_tracker.reset();

    while (true) {
      // This will return false if the HDR state changes or for any number of other
      // display or GPU changes. We should reinit to examine the updated state of
      // the display subsystem. It is recommended to call this once per frame.
      if (!factory->IsCurrent()) {
        return platf::capture_e::reinit;
      }

      platf::capture_e status = capture_e::ok;
      std::shared_ptr<img_t> img_out;

      // Try to continue frame pacing group, snapshot() is called with zero timeout after waiting for client frame interval
      if (frame_pacing_group_start) {
        const uint32_t seconds = (uint64_t) frame_pacing_group_frames * client_frame_rate_adjusted.Denominator / client_frame_rate_adjusted.Numerator;
        const uint32_t remainder = (uint64_t) frame_pacing_group_frames * client_frame_rate_adjusted.Denominator % client_frame_rate_adjusted.Numerator;
        const auto sleep_target = *frame_pacing_group_start +
                                  std::chrono::nanoseconds(1s) * seconds +
                                  std::chrono::nanoseconds(1s) * remainder / client_frame_rate_adjusted.Numerator;
        const auto sleep_period = sleep_target - std::chrono::steady_clock::now();

        if (sleep_period <= 0ns) {
          // We missed next frame time, invalidating current frame pacing group
          frame_pacing_group_start = std::nullopt;
          frame_pacing_group_frames = 0;
          status = capture_e::timeout;
        }
        else {
          high_precision_sleep(sleep_period);
          std::chrono::nanoseconds overshoot_ns = std::chrono::steady_clock::now() - sleep_target;
          log_sleep_overshoot(overshoot_ns);

          status = snapshot(pull_free_image_cb, img_out, 0ms, *cursor);

          if (status == capture_e::ok && img_out) {
            frame_pacing_group_frames += 1;
          }
          else {
            frame_pacing_group_start = std::nullopt;
            frame_pacing_group_frames = 0;
          }
        }
      }

      // Start new frame pacing group if necessary, snapshot() is called with non-zero timeout
      if (status == capture_e::timeout || (status == capture_e::ok && !frame_pacing_group_start)) {
        status = snapshot(pull_free_image_cb, img_out, 200ms, *cursor);

        if (status == capture_e::ok && img_out) {
          frame_pacing_group_start = img_out->frame_timestamp;

          if (!frame_pacing_group_start) {
            BOOST_LOG(warning) << "snapshot() provided image without timestamp";
            frame_pacing_group_start = std::chrono::steady_clock::now();
          }

          frame_pacing_group_frames = 1;
        }
        else if (status == platf::capture_e::timeout) {
          // The D3D11 device is protected by an unfair lock that is held the entire time that
          // IDXGIOutputDuplication::AcquireNextFrame() is running. This is normally harmless,
          // however sometimes the encoding thread needs to interact with our ID3D11Device to
          // create dummy images or initialize the shared state that is used to pass textures
          // between the capture and encoding ID3D11Devices.
          //
          // When we're in a state where we're not actively receiving frames regularly, we will
          // spend almost 100% of our time in AcquireNextFrame() holding that critical lock.
          // Worse still, since it's unfair, we can monopolize it while the encoding thread
          // is starved. The encoding thread may acquire it for a few moments across a few
          // ID3D11Device calls before losing it again to us for another long time waiting in
          // AcquireNextFrame(). The starvation caused by this lock contention causes encoder
          // reinitialization to take several seconds instead of a fraction of a second.
          //
          // To avoid starving the encoding thread, sleep without the lock held for a little
          // while each time we reach our max frame timeout. This will only happen when nothing
          // is updating the display, so no visible stutter should be introduced by the sleep.
          std::this_thread::sleep_for(10ms);
        }
      }

      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::interrupted:
          return status;
        case platf::capture_e::timeout:
          if (!push_captured_image_cb(std::move(img_out), false)) {
            return capture_e::ok;
          }
          break;
        case platf::capture_e::ok:
          if (!push_captured_image_cb(std::move(img_out), true)) {
            return capture_e::ok;
          }
          break;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return status;
      }

      status = dup.release_frame();
      if (status != platf::capture_e::ok) {
        return status;
      }
    }

    return capture_e::ok;
  }

  bool
  set_gpu_preference_on_self(int preference) {
    // The GPU preferences key uses app path as the value name.
    WCHAR sunshine_path[MAX_PATH];
    GetModuleFileNameW(NULL, sunshine_path, ARRAYSIZE(sunshine_path));

    WCHAR value_data[128];
    swprintf_s(value_data, L"GpuPreference=%d;", preference);

    auto status = RegSetKeyValueW(HKEY_CURRENT_USER,
      L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
      sunshine_path,
      REG_SZ,
      value_data,
      (wcslen(value_data) + 1) * sizeof(WCHAR));
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(error) << "Failed to set GPU preference: "sv << status;
      return false;
    }

    BOOST_LOG(info) << "Set GPU preference: "sv << preference;
    return true;
  }

  // On hybrid graphics systems, Windows will change the order of GPUs reported by
  // DXGI in accordance with the user's GPU preference. If the selected GPU is a
  // render-only device with no displays, DXGI will add virtual outputs to the
  // that device to avoid confusing applications. While this works properly for most
  // applications, it breaks the Desktop Duplication API because DXGI doesn't proxy
  // the virtual DXGIOutput to the real GPU it is attached to. When trying to call
  // DuplicateOutput() on one of these virtual outputs, it fails with DXGI_ERROR_UNSUPPORTED
  // (even if you try sneaky stuff like passing the ID3D11Device for the iGPU and the
  // virtual DXGIOutput from the dGPU). Because the GPU preference is once-per-process,
  // we spawn a helper tool to probe for us before we set our own GPU preference.
  bool
  probe_for_gpu_preference(const std::string &display_name) {
    // If we've already been through here, there's nothing to do this time.
    static bool set_gpu_preference = false;
    if (set_gpu_preference) {
      return true;
    }

    std::string cmd = "tools\\ddprobe.exe";

    // We start at 1 because 0 is automatic selection which can be overridden by
    // the GPU driver control panel options. Since ddprobe.exe can have different
    // GPU driver overrides than Sunshine.exe, we want to avoid a scenario where
    // autoselection might work for ddprobe.exe but not for us.
    for (int i = 1; i < 5; i++) {
      // Run the probe tool. It returns the status of DuplicateOutput().
      //
      // Arg format: [GPU preference] [Display name]
      HRESULT result;
      try {
        result = bp::system(cmd, std::to_string(i), display_name, bp::std_out > bp::null, bp::std_err > bp::null);
      }
      catch (bp::process_error &e) {
        BOOST_LOG(error) << "Failed to start ddprobe.exe: "sv << e.what();
        return false;
      }

      BOOST_LOG(info) << "ddprobe.exe ["sv << i << "] ["sv << display_name << "] returned: 0x"sv << util::hex(result).to_string_view();

      // E_ACCESSDENIED can happen at the login screen. If we get this error,
      // we know capture would have been supported, because DXGI_ERROR_UNSUPPORTED
      // would have been raised first if it wasn't.
      if (result == S_OK || result == E_ACCESSDENIED) {
        // We found a working GPU preference, so set ourselves to use that.
        if (set_gpu_preference_on_self(i)) {
          set_gpu_preference = true;
          return true;
        }
        else {
          return false;
        }
      }
      else {
        // This configuration didn't work, so continue testing others
        continue;
      }
    }

    // If none of the manual options worked, leave the GPU preference alone
    return false;
  }

  /**
   * @brief Tests to determine if the Desktop Duplication API can capture the given output.
   * @details When testing for enumeration only, we avoid resyncing the thread desktop.
   * @param adapter The DXGI adapter to use for capture.
   * @param output The DXGI output to capture.
   * @param enumeration_only Specifies whether this test is occurring for display enumeration.
   */
  bool
  test_dxgi_duplication(adapter_t &adapter, output_t &output, bool enumeration_only) {
    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    device_t device;
    auto status = D3D11CreateDevice(
      adapter.get(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels, sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      nullptr);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create D3D11 device for DD test [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    output1_t output1;
    status = output->QueryInterface(IID_IDXGIOutput1, (void **) &output1);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIOutput1 from the output"sv;
      return false;
    }

    // Check if we can use the Desktop Duplication API on this output
    for (int x = 0; x < 2; ++x) {
      dup_t dup;

      // Only resynchronize the thread desktop when not enumerating displays.
      // During enumeration, the caller will do this only once to ensure
      // a consistent view of available outputs.
      if (!enumeration_only) {
        syncThreadDesktop();
      }

      status = output1->DuplicateOutput((IUnknown *) device.get(), &dup);
      if (SUCCEEDED(status)) {
        return true;
      }

      // If we're not resyncing the thread desktop and we don't have permission to
      // capture the current desktop, just bail immediately. Retrying won't help.
      if (enumeration_only && status == E_ACCESSDENIED) {
        break;
      }
      else {
        std::this_thread::sleep_for(200ms);
      }
    }

    BOOST_LOG(error) << "DuplicateOutput() test failed [0x"sv << util::hex(status).to_string_view() << ']';
    return false;
  }

  int
  display_base_t::init(const ::video::config_t &config, const std::string &display_name) {
    std::once_flag windows_cpp_once_flag;

    std::call_once(windows_cpp_once_flag, []() {
      DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);

      typedef BOOL (*User32_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);

      auto user32 = LoadLibraryA("user32.dll");
      auto f = (User32_SetProcessDpiAwarenessContext) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
      if (f) {
        f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      }

      FreeLibrary(user32);
    });

    // Get rectangle of full desktop for absolute mouse coordinates
    env_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    env_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HRESULT status;

    // We must set the GPU preference before calling any DXGI APIs!
    if (!probe_for_gpu_preference(display_name)) {
      BOOST_LOG(warning) << "Failed to set GPU preference. Capture may not work!"sv;
    }

    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    auto adapter_name = from_utf8(config::video.adapter_name);
    auto output_name = from_utf8(display_name);

    adapter_t::pointer adapter_p;
    for (int tries = 0; tries < 2; ++tries) {
      for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
        dxgi::adapter_t adapter_tmp { adapter_p };

        DXGI_ADAPTER_DESC1 adapter_desc;
        adapter_tmp->GetDesc1(&adapter_desc);

        if (!adapter_name.empty() && adapter_desc.Description != adapter_name) {
          continue;
        }

        dxgi::output_t::pointer output_p;
        for (int y = 0; adapter_tmp->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
          dxgi::output_t output_tmp { output_p };

          DXGI_OUTPUT_DESC desc;
          output_tmp->GetDesc(&desc);

          if (!output_name.empty() && desc.DeviceName != output_name) {
            continue;
          }

          if (desc.AttachedToDesktop && test_dxgi_duplication(adapter_tmp, output_tmp, false)) {
            output = std::move(output_tmp);

            offset_x = desc.DesktopCoordinates.left;
            offset_y = desc.DesktopCoordinates.top;
            width = desc.DesktopCoordinates.right - offset_x;
            height = desc.DesktopCoordinates.bottom - offset_y;

            display_rotation = desc.Rotation;
            if (display_rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                display_rotation == DXGI_MODE_ROTATION_ROTATE270) {
              width_before_rotation = height;
              height_before_rotation = width;
            }
            else {
              width_before_rotation = width;
              height_before_rotation = height;
            }

            // left and bottom may be negative, yet absolute mouse coordinates start at 0x0
            // Ensure offset starts at 0x0
            offset_x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
            offset_y -= GetSystemMetrics(SM_YVIRTUALSCREEN);
          }
        }

        if (output) {
          adapter = std::move(adapter_tmp);
          break;
        }
      }

      if (output) {
        break;
      }

      // If we made it here without finding an output, try to power on the display and retry.
      if (tries == 0) {
        SetThreadExecutionState(ES_DISPLAY_REQUIRED);
        Sleep(500);
      }
    }

    if (!output) {
      BOOST_LOG(error) << "Failed to locate an output device"sv;
      return -1;
    }

    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    status = adapter->QueryInterface(IID_IDXGIAdapter, (void **) &adapter_p);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIAdapter interface"sv;
      return -1;
    }

    status = D3D11CreateDevice(
      adapter_p,
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels, sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &device,
      &feature_level,
      &device_ctx);

    adapter_p->Release();

    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';

      return -1;
    }

    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    auto description = to_utf8(adapter_desc.Description);
    BOOST_LOG(info)
      << std::endl
      << "Device Description : " << description << std::endl
      << "Device Vendor ID   : 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << std::endl
      << "Device Device ID   : 0x"sv << util::hex(adapter_desc.DeviceId).to_string_view() << std::endl
      << "Device Video Mem   : "sv << adapter_desc.DedicatedVideoMemory / 1048576 << " MiB"sv << std::endl
      << "Device Sys Mem     : "sv << adapter_desc.DedicatedSystemMemory / 1048576 << " MiB"sv << std::endl
      << "Share Sys Mem      : "sv << adapter_desc.SharedSystemMemory / 1048576 << " MiB"sv << std::endl
      << "Feature Level      : 0x"sv << util::hex(feature_level).to_string_view() << std::endl
      << "Capture size       : "sv << width << 'x' << height << std::endl
      << "Offset             : "sv << offset_x << 'x' << offset_y << std::endl
      << "Virtual Desktop    : "sv << env_width << 'x' << env_height;

    // Bump up thread priority
    {
      const DWORD flags = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
      TOKEN_PRIVILEGES tp;
      HANDLE token;
      LUID val;

      if (OpenProcessToken(GetCurrentProcess(), flags, &token) &&
          !!LookupPrivilegeValue(NULL, SE_INC_BASE_PRIORITY_NAME, &val)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = val;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!AdjustTokenPrivileges(token, false, &tp, sizeof(tp), NULL, NULL)) {
          BOOST_LOG(warning) << "Could not set privilege to increase GPU priority";
        }
      }

      CloseHandle(token);

      HMODULE gdi32 = GetModuleHandleA("GDI32");
      if (gdi32) {
        auto check_hags = [&](const LUID &adapter) -> bool {
          auto d3dkmt_open_adapter = (PD3DKMTOpenAdapterFromLuid) GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
          auto d3dkmt_query_adapter_info = (PD3DKMTQueryAdapterInfo) GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
          auto d3dkmt_close_adapter = (PD3DKMTCloseAdapter) GetProcAddress(gdi32, "D3DKMTCloseAdapter");
          if (!d3dkmt_open_adapter || !d3dkmt_query_adapter_info || !d3dkmt_close_adapter) {
            BOOST_LOG(error) << "Couldn't load d3dkmt functions from gdi32.dll to determine GPU HAGS status";
            return false;
          }

          D3DKMT_OPENADAPTERFROMLUID d3dkmt_adapter = { adapter };
          if (FAILED(d3dkmt_open_adapter(&d3dkmt_adapter))) {
            BOOST_LOG(error) << "D3DKMTOpenAdapterFromLuid() failed while trying to determine GPU HAGS status";
            return false;
          }

          bool result;

          D3DKMT_WDDM_2_7_CAPS d3dkmt_adapter_caps = {};
          D3DKMT_QUERYADAPTERINFO d3dkmt_adapter_info = {};
          d3dkmt_adapter_info.hAdapter = d3dkmt_adapter.hAdapter;
          d3dkmt_adapter_info.Type = KMTQAITYPE_WDDM_2_7_CAPS;
          d3dkmt_adapter_info.pPrivateDriverData = &d3dkmt_adapter_caps;
          d3dkmt_adapter_info.PrivateDriverDataSize = sizeof(d3dkmt_adapter_caps);

          if (SUCCEEDED(d3dkmt_query_adapter_info(&d3dkmt_adapter_info))) {
            result = d3dkmt_adapter_caps.HwSchEnabled;
          }
          else {
            BOOST_LOG(warning) << "D3DKMTQueryAdapterInfo() failed while trying to determine GPU HAGS status";
            result = false;
          }

          D3DKMT_CLOSEADAPTER d3dkmt_close_adapter_wrap = { d3dkmt_adapter.hAdapter };
          if (FAILED(d3dkmt_close_adapter(&d3dkmt_close_adapter_wrap))) {
            BOOST_LOG(error) << "D3DKMTCloseAdapter() failed while trying to determine GPU HAGS status";
          }

          return result;
        };

        auto d3dkmt_set_process_priority = (PD3DKMTSetProcessSchedulingPriorityClass) GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass");
        if (d3dkmt_set_process_priority) {
          auto priority = D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
          bool hags_enabled = check_hags(adapter_desc.AdapterLuid);
          if (adapter_desc.VendorId == 0x10DE) {
            // As of 2023.07, NVIDIA driver has unfixed bug(s) where "realtime" can cause unrecoverable encoding freeze or outright driver crash
            // This issue happens more frequently with HAGS, in DX12 games or when VRAM is filled close to max capacity
            // Track OBS to see if they find better workaround or NVIDIA fixes it on their end, they seem to be in communication
            if (hags_enabled && !config::video.nv_realtime_hags) priority = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH;
          }
          BOOST_LOG(info) << "Active GPU has HAGS " << (hags_enabled ? "enabled" : "disabled");
          BOOST_LOG(info) << "Using " << (priority == D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH ? "high" : "realtime") << " GPU priority";
          if (FAILED(d3dkmt_set_process_priority(GetCurrentProcess(), priority))) {
            BOOST_LOG(warning) << "Failed to adjust GPU priority. Please run application as administrator for optimal performance.";
          }
        }
        else {
          BOOST_LOG(error) << "Couldn't load D3DKMTSetProcessSchedulingPriorityClass function from gdi32.dll to adjust GPU priority";
        }
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase capture GPU thread priority. Please run application as administrator for optimal performance.";
      }
    }

    // Try to reduce latency
    {
      dxgi::dxgi1_t dxgi {};
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetMaximumFrameLatency(1);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to set maximum frame latency [0x"sv << util::hex(status).to_string_view() << ']';
      }
    }

    // FIXME: Duplicate output on RX580 in combination with DOOM (2016) --> BSOD
    {
      // IDXGIOutput5 is optional, but can provide improved performance and wide color support
      dxgi::output5_t output5 {};
      status = output->QueryInterface(IID_IDXGIOutput5, (void **) &output5);
      if (SUCCEEDED(status)) {
        // Ask the display implementation which formats it supports
        auto supported_formats = get_supported_capture_formats();
        if (supported_formats.empty()) {
          BOOST_LOG(warning) << "No compatible capture formats for this encoder"sv;
          return -1;
        }

        // We try this twice, in case we still get an error on reinitialization
        for (int x = 0; x < 2; ++x) {
          // Ensure we can duplicate the current display
          syncThreadDesktop();

          status = output5->DuplicateOutput1((IUnknown *) device.get(), 0, supported_formats.size(), supported_formats.data(), &dup.dup);
          if (SUCCEEDED(status)) {
            break;
          }
          std::this_thread::sleep_for(200ms);
        }

        // We don't retry with DuplicateOutput() because we can hit this codepath when we're racing
        // with mode changes and we don't want to accidentally fall back to suboptimal capture if
        // we get unlucky and succeed below.
        if (FAILED(status)) {
          BOOST_LOG(warning) << "DuplicateOutput1 Failed [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      }
      else {
        BOOST_LOG(warning) << "IDXGIOutput5 is not supported by your OS. Capture performance may be reduced."sv;

        dxgi::output1_t output1 {};
        status = output->QueryInterface(IID_IDXGIOutput1, (void **) &output1);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to query IDXGIOutput1 from the output"sv;
          return -1;
        }

        for (int x = 0; x < 2; ++x) {
          // Ensure we can duplicate the current display
          syncThreadDesktop();

          status = output1->DuplicateOutput((IUnknown *) device.get(), &dup.dup);
          if (SUCCEEDED(status)) {
            break;
          }
          std::this_thread::sleep_for(200ms);
        }

        if (FAILED(status)) {
          BOOST_LOG(error) << "DuplicateOutput Failed [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      }
    }

    DXGI_OUTDUPL_DESC dup_desc;
    dup.dup->GetDesc(&dup_desc);

    BOOST_LOG(info) << "Desktop resolution ["sv << dup_desc.ModeDesc.Width << 'x' << dup_desc.ModeDesc.Height << ']';
    BOOST_LOG(info) << "Desktop format ["sv << dxgi_format_to_string(dup_desc.ModeDesc.Format) << ']';

    display_refresh_rate = dup_desc.ModeDesc.RefreshRate;
    double display_refresh_rate_decimal = (double) display_refresh_rate.Numerator / display_refresh_rate.Denominator;
    BOOST_LOG(info) << "Display refresh rate [" << display_refresh_rate_decimal << "Hz]";
    display_refresh_rate_rounded = lround(display_refresh_rate_decimal);

    client_frame_rate = config.framerate;
    BOOST_LOG(info) << "Requested frame rate [" << client_frame_rate << "fps]";

    dxgi::output6_t output6 {};
    status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (SUCCEEDED(status)) {
      DXGI_OUTPUT_DESC1 desc1;
      output6->GetDesc1(&desc1);

      BOOST_LOG(info)
        << std::endl
        << "Colorspace         : "sv << colorspace_to_string(desc1.ColorSpace) << std::endl
        << "Bits Per Color     : "sv << desc1.BitsPerColor << std::endl
        << "Red Primary        : ["sv << desc1.RedPrimary[0] << ',' << desc1.RedPrimary[1] << ']' << std::endl
        << "Green Primary      : ["sv << desc1.GreenPrimary[0] << ',' << desc1.GreenPrimary[1] << ']' << std::endl
        << "Blue Primary       : ["sv << desc1.BluePrimary[0] << ',' << desc1.BluePrimary[1] << ']' << std::endl
        << "White Point        : ["sv << desc1.WhitePoint[0] << ',' << desc1.WhitePoint[1] << ']' << std::endl
        << "Min Luminance      : "sv << desc1.MinLuminance << " nits"sv << std::endl
        << "Max Luminance      : "sv << desc1.MaxLuminance << " nits"sv << std::endl
        << "Max Full Luminance : "sv << desc1.MaxFullFrameLuminance << " nits"sv;
    }

    // Capture format will be determined from the first call to AcquireNextFrame()
    capture_format = DXGI_FORMAT_UNKNOWN;

    // Use CREATE_WAITABLE_TIMER_HIGH_RESOLUTION if supported (Windows 10 1809+)
    timer.reset(CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!timer) {
      timer.reset(CreateWaitableTimerEx(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
      if (!timer) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "Failed to create timer: "sv << winerr;
        return -1;
      }
    }

    return 0;
  }

  bool
  display_base_t::is_hdr() {
    dxgi::output6_t output6 {};

    auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Failed to query IDXGIOutput6 from the output"sv;
      return false;
    }

    DXGI_OUTPUT_DESC1 desc1;
    output6->GetDesc1(&desc1);

    return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  }

  bool
  display_base_t::get_hdr_metadata(SS_HDR_METADATA &metadata) {
    dxgi::output6_t output6 {};

    std::memset(&metadata, 0, sizeof(metadata));

    auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Failed to query IDXGIOutput6 from the output"sv;
      return false;
    }

    DXGI_OUTPUT_DESC1 desc1;
    output6->GetDesc1(&desc1);

    // The primaries reported here seem to correspond to scRGB (Rec. 709)
    // which we then convert to Rec 2020 in our scRGB FP16 -> PQ shader
    // prior to encoding. It's not clear to me if we're supposed to report
    // the primaries of the original colorspace or the one we've converted
    // it to, but let's just report Rec 2020 primaries and D65 white level
    // to avoid confusing clients by reporting Rec 709 primaries with a
    // Rec 2020 colorspace. It seems like most clients ignore the primaries
    // in the metadata anyway (luminance range is most important).
    desc1.RedPrimary[0] = 0.708f;
    desc1.RedPrimary[1] = 0.292f;
    desc1.GreenPrimary[0] = 0.170f;
    desc1.GreenPrimary[1] = 0.797f;
    desc1.BluePrimary[0] = 0.131f;
    desc1.BluePrimary[1] = 0.046f;
    desc1.WhitePoint[0] = 0.3127f;
    desc1.WhitePoint[1] = 0.3290f;

    metadata.displayPrimaries[0].x = desc1.RedPrimary[0] * 50000;
    metadata.displayPrimaries[0].y = desc1.RedPrimary[1] * 50000;
    metadata.displayPrimaries[1].x = desc1.GreenPrimary[0] * 50000;
    metadata.displayPrimaries[1].y = desc1.GreenPrimary[1] * 50000;
    metadata.displayPrimaries[2].x = desc1.BluePrimary[0] * 50000;
    metadata.displayPrimaries[2].y = desc1.BluePrimary[1] * 50000;

    metadata.whitePoint.x = desc1.WhitePoint[0] * 50000;
    metadata.whitePoint.y = desc1.WhitePoint[1] * 50000;

    metadata.maxDisplayLuminance = desc1.MaxLuminance;
    metadata.minDisplayLuminance = desc1.MinLuminance * 10000;

    // These are content-specific metadata parameters that this interface doesn't give us
    metadata.maxContentLightLevel = 0;
    metadata.maxFrameAverageLightLevel = 0;

    metadata.maxFullFrameLuminance = desc1.MaxFullFrameLuminance;

    return true;
  }

  const char *format_str[] = {
    "DXGI_FORMAT_UNKNOWN",
    "DXGI_FORMAT_R32G32B32A32_TYPELESS",
    "DXGI_FORMAT_R32G32B32A32_FLOAT",
    "DXGI_FORMAT_R32G32B32A32_UINT",
    "DXGI_FORMAT_R32G32B32A32_SINT",
    "DXGI_FORMAT_R32G32B32_TYPELESS",
    "DXGI_FORMAT_R32G32B32_FLOAT",
    "DXGI_FORMAT_R32G32B32_UINT",
    "DXGI_FORMAT_R32G32B32_SINT",
    "DXGI_FORMAT_R16G16B16A16_TYPELESS",
    "DXGI_FORMAT_R16G16B16A16_FLOAT",
    "DXGI_FORMAT_R16G16B16A16_UNORM",
    "DXGI_FORMAT_R16G16B16A16_UINT",
    "DXGI_FORMAT_R16G16B16A16_SNORM",
    "DXGI_FORMAT_R16G16B16A16_SINT",
    "DXGI_FORMAT_R32G32_TYPELESS",
    "DXGI_FORMAT_R32G32_FLOAT",
    "DXGI_FORMAT_R32G32_UINT",
    "DXGI_FORMAT_R32G32_SINT",
    "DXGI_FORMAT_R32G8X24_TYPELESS",
    "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
    "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
    "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
    "DXGI_FORMAT_R10G10B10A2_TYPELESS",
    "DXGI_FORMAT_R10G10B10A2_UNORM",
    "DXGI_FORMAT_R10G10B10A2_UINT",
    "DXGI_FORMAT_R11G11B10_FLOAT",
    "DXGI_FORMAT_R8G8B8A8_TYPELESS",
    "DXGI_FORMAT_R8G8B8A8_UNORM",
    "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
    "DXGI_FORMAT_R8G8B8A8_UINT",
    "DXGI_FORMAT_R8G8B8A8_SNORM",
    "DXGI_FORMAT_R8G8B8A8_SINT",
    "DXGI_FORMAT_R16G16_TYPELESS",
    "DXGI_FORMAT_R16G16_FLOAT",
    "DXGI_FORMAT_R16G16_UNORM",
    "DXGI_FORMAT_R16G16_UINT",
    "DXGI_FORMAT_R16G16_SNORM",
    "DXGI_FORMAT_R16G16_SINT",
    "DXGI_FORMAT_R32_TYPELESS",
    "DXGI_FORMAT_D32_FLOAT",
    "DXGI_FORMAT_R32_FLOAT",
    "DXGI_FORMAT_R32_UINT",
    "DXGI_FORMAT_R32_SINT",
    "DXGI_FORMAT_R24G8_TYPELESS",
    "DXGI_FORMAT_D24_UNORM_S8_UINT",
    "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
    "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
    "DXGI_FORMAT_R8G8_TYPELESS",
    "DXGI_FORMAT_R8G8_UNORM",
    "DXGI_FORMAT_R8G8_UINT",
    "DXGI_FORMAT_R8G8_SNORM",
    "DXGI_FORMAT_R8G8_SINT",
    "DXGI_FORMAT_R16_TYPELESS",
    "DXGI_FORMAT_R16_FLOAT",
    "DXGI_FORMAT_D16_UNORM",
    "DXGI_FORMAT_R16_UNORM",
    "DXGI_FORMAT_R16_UINT",
    "DXGI_FORMAT_R16_SNORM",
    "DXGI_FORMAT_R16_SINT",
    "DXGI_FORMAT_R8_TYPELESS",
    "DXGI_FORMAT_R8_UNORM",
    "DXGI_FORMAT_R8_UINT",
    "DXGI_FORMAT_R8_SNORM",
    "DXGI_FORMAT_R8_SINT",
    "DXGI_FORMAT_A8_UNORM",
    "DXGI_FORMAT_R1_UNORM",
    "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
    "DXGI_FORMAT_R8G8_B8G8_UNORM",
    "DXGI_FORMAT_G8R8_G8B8_UNORM",
    "DXGI_FORMAT_BC1_TYPELESS",
    "DXGI_FORMAT_BC1_UNORM",
    "DXGI_FORMAT_BC1_UNORM_SRGB",
    "DXGI_FORMAT_BC2_TYPELESS",
    "DXGI_FORMAT_BC2_UNORM",
    "DXGI_FORMAT_BC2_UNORM_SRGB",
    "DXGI_FORMAT_BC3_TYPELESS",
    "DXGI_FORMAT_BC3_UNORM",
    "DXGI_FORMAT_BC3_UNORM_SRGB",
    "DXGI_FORMAT_BC4_TYPELESS",
    "DXGI_FORMAT_BC4_UNORM",
    "DXGI_FORMAT_BC4_SNORM",
    "DXGI_FORMAT_BC5_TYPELESS",
    "DXGI_FORMAT_BC5_UNORM",
    "DXGI_FORMAT_BC5_SNORM",
    "DXGI_FORMAT_B5G6R5_UNORM",
    "DXGI_FORMAT_B5G5R5A1_UNORM",
    "DXGI_FORMAT_B8G8R8A8_UNORM",
    "DXGI_FORMAT_B8G8R8X8_UNORM",
    "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
    "DXGI_FORMAT_B8G8R8A8_TYPELESS",
    "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
    "DXGI_FORMAT_B8G8R8X8_TYPELESS",
    "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
    "DXGI_FORMAT_BC6H_TYPELESS",
    "DXGI_FORMAT_BC6H_UF16",
    "DXGI_FORMAT_BC6H_SF16",
    "DXGI_FORMAT_BC7_TYPELESS",
    "DXGI_FORMAT_BC7_UNORM",
    "DXGI_FORMAT_BC7_UNORM_SRGB",
    "DXGI_FORMAT_AYUV",
    "DXGI_FORMAT_Y410",
    "DXGI_FORMAT_Y416",
    "DXGI_FORMAT_NV12",
    "DXGI_FORMAT_P010",
    "DXGI_FORMAT_P016",
    "DXGI_FORMAT_420_OPAQUE",
    "DXGI_FORMAT_YUY2",
    "DXGI_FORMAT_Y210",
    "DXGI_FORMAT_Y216",
    "DXGI_FORMAT_NV11",
    "DXGI_FORMAT_AI44",
    "DXGI_FORMAT_IA44",
    "DXGI_FORMAT_P8",
    "DXGI_FORMAT_A8P8",
    "DXGI_FORMAT_B4G4R4A4_UNORM",

    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

    "DXGI_FORMAT_P208",
    "DXGI_FORMAT_V208",
    "DXGI_FORMAT_V408"
  };

  const char *
  display_base_t::dxgi_format_to_string(DXGI_FORMAT format) {
    return format_str[format];
  }

  const char *
  display_base_t::colorspace_to_string(DXGI_COLOR_SPACE_TYPE type) {
    const char *type_str[] = {
      "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020",
      "DXGI_COLOR_SPACE_RESERVED",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020",
    };

    if (type < ARRAYSIZE(type_str)) {
      return type_str[type];
    }
    else {
      return "UNKNOWN";
    }
  }

}  // namespace platf::dxgi

namespace platf {
  std::shared_ptr<display_t>
  display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type == mem_type_e::dxgi) {
      auto disp = std::make_shared<dxgi::display_vram_t>();

      if (!disp->init(config, display_name)) {
        return disp;
      }
    }
    else if (hwdevice_type == mem_type_e::system) {
      auto disp = std::make_shared<dxgi::display_ram_t>();

      if (!disp->init(config, display_name)) {
        return disp;
      }
    }

    return nullptr;
  }

  std::vector<std::string>
  display_names(mem_type_e) {
    std::vector<std::string> display_names;

    HRESULT status;

    BOOST_LOG(debug) << "Detecting monitors..."sv;

    // We must set the GPU preference before calling any DXGI APIs!
    if (!dxgi::probe_for_gpu_preference(config::video.output_name)) {
      BOOST_LOG(warning) << "Failed to set GPU preference. Capture may not work!"sv;
    }

    // We sync the thread desktop once before we start the enumeration process
    // to ensure test_dxgi_duplication() returns consistent results for all GPUs
    // even if the current desktop changes during our enumeration process.
    // It is critical that we either fully succeed in enumeration or fully fail,
    // otherwise it can lead to the capture code switching monitors unexpectedly.
    syncThreadDesktop();

    dxgi::factory1_t factory;
    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return {};
    }

    dxgi::adapter_t adapter;
    for (int x = 0; factory->EnumAdapters1(x, &adapter) != DXGI_ERROR_NOT_FOUND; ++x) {
      DXGI_ADAPTER_DESC1 adapter_desc;
      adapter->GetDesc1(&adapter_desc);

      BOOST_LOG(debug)
        << std::endl
        << "====== ADAPTER ====="sv << std::endl
        << "Device Name      : "sv << to_utf8(adapter_desc.Description) << std::endl
        << "Device Vendor ID : 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << std::endl
        << "Device Device ID : 0x"sv << util::hex(adapter_desc.DeviceId).to_string_view() << std::endl
        << "Device Video Mem : "sv << adapter_desc.DedicatedVideoMemory / 1048576 << " MiB"sv << std::endl
        << "Device Sys Mem   : "sv << adapter_desc.DedicatedSystemMemory / 1048576 << " MiB"sv << std::endl
        << "Share Sys Mem    : "sv << adapter_desc.SharedSystemMemory / 1048576 << " MiB"sv << std::endl
        << std::endl
        << "    ====== OUTPUT ======"sv << std::endl;

      dxgi::output_t::pointer output_p {};
      for (int y = 0; adapter->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
        dxgi::output_t output { output_p };

        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        auto device_name = to_utf8(desc.DeviceName);

        auto width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        auto height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        BOOST_LOG(debug)
          << "    Output Name       : "sv << device_name << std::endl
          << "    AttachedToDesktop : "sv << (desc.AttachedToDesktop ? "yes"sv : "no"sv) << std::endl
          << "    Resolution        : "sv << width << 'x' << height << std::endl
          << std::endl;

        // Don't include the display in the list if we can't actually capture it
        if (desc.AttachedToDesktop && dxgi::test_dxgi_duplication(adapter, output, true)) {
          display_names.emplace_back(std::move(device_name));
        }
      }
    }

    return display_names;
  }

  std::vector<config::display_options_t>
  display_options() {
    // We sync the thread desktop once before we start the enumeration process
    // to ensure test_dxgi_duplication() returns consistent results for all GPUs
    // even if the current desktop changes during our enumeration process.
    // It is critical that we either fully succeed in enumeration or fully fail,
    // otherwise it can lead to the capture code switching monitors unexpectedly.
    syncThreadDesktop();

    dxgi::factory1_t factory;
    auto status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      return {};
    }

    std::vector<config::display_options_t> display_options;

    dxgi::adapter_t adapter;
    int monitorIndex = 0;
    for (int x = 0; factory->EnumAdapters1(x, &adapter) != DXGI_ERROR_NOT_FOUND; ++x) {
      DXGI_ADAPTER_DESC1 adapter_desc;
      adapter->GetDesc1(&adapter_desc);

      dxgi::output_t::pointer output_p {};
      for (int y = 0; adapter->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
        dxgi::output_t output { output_p };

        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        auto device_name = to_utf8(desc.DeviceName);

        // Don't include the display in the list if we can't actually capture it
        if (desc.AttachedToDesktop && dxgi::test_dxgi_duplication(adapter, output, true)) {
          auto option = config::display_options_t {
            monitorIndex++,
            std::move(device_name),
            false // TODO: Correclty check if this is the primary display for windows, idk how
          };
          display_options.emplace_back(option);
        }
      }
    }

    return display_options;
  }

  /**
   * @brief Returns if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool
  needs_encoder_reenumeration() {
    // Serialize access to the static DXGI factory
    static std::mutex reenumeration_state_lock;
    auto lg = std::lock_guard(reenumeration_state_lock);

    // Keep a reference to the DXGI factory, which will keep track of changes internally.
    static dxgi::factory1_t factory;
    if (!factory || !factory->IsCurrent()) {
      factory.reset();

      auto status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
        factory.release();
      }

      // Always request reenumeration on the first streaming session just to ensure we
      // can deal with any initialization races that may occur when the system is booting.
      BOOST_LOG(info) << "Encoder reenumeration is required"sv;
      return true;
    }
    else {
      // The DXGI factory from last time is still current, so no encoder changes have occurred.
      return false;
    }
  }
}  // namespace platf
