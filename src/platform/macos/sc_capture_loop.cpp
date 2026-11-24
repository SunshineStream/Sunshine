/**
 * @file src/platform/macos/sc_capture_loop.cpp
 * @brief Definitions for the ScreenCaptureKit capture loop and backend fallback policy on macOS.
 */
// standard includes
#include <memory>
#include <utility>

// local includes
#include "src/platform/macos/sc_capture_loop.h"
#include "src/utility.h"

namespace platf::sckit {
  void backend_policy_t::note_stream_start_failure() {
    _stream_start_failed = true;
  }

  bool backend_policy_t::stream_start_failed() const {
    return _stream_start_failed;
  }

  bool backend_policy_t::prefer_sckit(bool sckit_available) const {
    return sckit_available && !_stream_start_failed;
  }

  backend_policy_t &backend_policy() {
    static backend_policy_t policy;
    return policy;
  }

  capture_e run_capture_loop(
    frame_source_t &source,
    backend_policy_t &policy,
    const display_t::push_captured_image_cb_t &push_captured_image_cb,
    const display_t::pull_free_image_cb_t &pull_free_image_cb,
    const process_frame_fn_t &process_frame,
    std::chrono::nanoseconds poll_interval
  ) {
    if (!source.start()) {
      // The display was built for ScreenCaptureKit but the stream never came up: ask the
      // capture thread to rebuild the display, which the policy now steers to AVFoundation.
      policy.note_stream_start_failure();
      return capture_e::reinit;
    }

    auto stop_source = util::fail_guard([&source]() {
      source.stop();
    });

    while (true) {
      // The stream pushes frames on its own; wait for the next one, waking periodically
      // to report a timeout and to notice interrupt requests.
      source.wait_frame(poll_interval);

      if (source.stopped()) {
        // The stream died underneath us (display change, permission revoked, ...);
        // only a display rebuild gets capture going again.
        return capture_e::reinit;
      }

      CMSampleBufferRef sample_buffer = source.copy_latest_sample_buffer();

      if (!sample_buffer) {
        // No new frame within the poll interval: report a timeout like the other backends
        // so the pipeline can repeat the previous frame.
        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out) || !push_captured_image_cb(std::move(img_out), false)) {
          return capture_e::ok;
        }
        continue;
      }

      auto release_sample_buffer = util::fail_guard([sample_buffer]() {
        CFRelease(sample_buffer);
      });

      std::shared_ptr<img_t> img_out;
      if (!pull_free_image_cb(img_out)) {
        return capture_e::ok;
      }

      if (!process_frame(sample_buffer, img_out.get())) {
        continue;
      }

      if (!push_captured_image_cb(std::move(img_out), true)) {
        return capture_e::ok;
      }
    }
  }
}  // namespace platf::sckit
