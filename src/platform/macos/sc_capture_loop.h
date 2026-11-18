/**
 * @file src/platform/macos/sc_capture_loop.h
 * @brief Declarations for the ScreenCaptureKit capture loop and backend fallback policy on macOS.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <functional>

// platform includes
#include <CoreMedia/CoreMedia.h>

// local includes
#include "src/platform/common.h"

namespace platf::sckit {
  /**
   * @brief Process-wide policy deciding whether ScreenCaptureKit is still the preferred capture backend.
   *
   * ScreenCaptureKit is selected when a display is created, but its stream can only be started once
   * capture begins. A stream that fails to start flips this policy so that the display is rebuilt
   * on AVFoundation instead.
   */
  class backend_policy_t {
  public:
    /**
     * @brief Record that an SCStream could not be started.
     */
    void note_stream_start_failure();

    /**
     * @brief Check whether an SCStream previously failed to start in this process.
     *
     * @return `true` once a stream start failure was recorded.
     */
    [[nodiscard]] bool stream_start_failed() const;

    /**
     * @brief Decide whether ScreenCaptureKit should be used for a new display.
     *
     * @param sckit_available Whether the running system supports ScreenCaptureKit capture.
     * @return `true` when ScreenCaptureKit is available and has not failed to start a stream.
     */
    [[nodiscard]] bool prefer_sckit(bool sckit_available) const;

  private:
    std::atomic<bool> _stream_start_failed {false};  ///< Set once an SCStream fails to start.
  };

  /**
   * @brief Access the process-wide backend policy.
   *
   * @return Shared policy instance.
   */
  backend_policy_t &backend_policy();

  /**
   * @brief Minimal view of an SCStream as seen by the capture loop.
   *
   * The loop only needs to start and stop the stream, wait for and take the latest frame, and
   * learn when the stream stopped on its own, so tests can drive it with a scripted source.
   */
  class frame_source_t {
  public:
    virtual ~frame_source_t() = default;

    /**
     * @brief Start the stream.
     *
     * @return `true` when frames may be expected; `false` when the stream could not be started.
     */
    virtual bool start() = 0;

    /**
     * @brief Wait until a new frame is available or the timeout expires.
     *
     * @param timeout Maximum time to wait.
     */
    virtual void wait_frame(std::chrono::nanoseconds timeout) = 0;

    /**
     * @brief Check whether the stream stopped without being asked to.
     *
     * @return `true` once the stream reported an unexpected stop.
     */
    [[nodiscard]] virtual bool stopped() const = 0;

    /**
     * @brief Take ownership of the most recent frame.
     *
     * @return Retained sample buffer, or `NULL` when no new frame arrived.
     */
    virtual CMSampleBufferRef copy_latest_sample_buffer() = 0;

    /**
     * @brief Stop the stream and release its resources.
     */
    virtual void stop() = 0;
  };

  /**
   * @brief Callback that converts a sample buffer into a Sunshine image.
   *
   * Returns `false` when the sample buffer carried no usable pixel buffer.
   */
  using process_frame_fn_t = std::function<bool(CMSampleBufferRef sample_buffer, img_t *img)>;

  /**
   * @brief Run the ScreenCaptureKit capture loop until the pipeline interrupts it or the stream fails.
   *
   * Frames are pushed as they arrive; a poll interval without a frame is reported as a timeout so
   * the pipeline can repeat the previous frame. A stream that fails to start or stops on its own
   * yields `capture_e::reinit`, which makes the capture thread rebuild the display; a start failure
   * also updates the backend policy, so that rebuild falls back to AVFoundation.
   * @param source Stream to capture from; started here and stopped before returning unless the start failed.
   * @param policy Backend policy updated when the stream cannot be started.
   * @param push_captured_image_cb Pipeline callback receiving captured or repeated images.
   * @param pull_free_image_cb Pipeline callback providing free images.
   * @param process_frame Converts a sample buffer into the pulled image.
   * @param poll_interval Longest time to wait for a frame before reporting a timeout.
   * @return `capture_e::ok` when the pipeline interrupted capture; `capture_e::reinit` when the stream failed.
   */
  capture_e run_capture_loop(
    frame_source_t &source,
    backend_policy_t &policy,
    const display_t::push_captured_image_cb_t &push_captured_image_cb,
    const display_t::pull_free_image_cb_t &pull_free_image_cb,
    const process_frame_fn_t &process_frame,
    std::chrono::nanoseconds poll_interval
  );
}  // namespace platf::sckit
