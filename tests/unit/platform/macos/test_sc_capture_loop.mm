/**
 * @file tests/unit/platform/macos/test_sc_capture_loop.mm
 * @brief Unit tests for src/platform/macos/sc_capture_loop.* and the SCCapture stop delegate.
 */
// Only compile these tests on macOS
#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <deque>
  #include <memory>
  #include <vector>

  // platform includes
  #import <CoreMedia/CoreMedia.h>
  #import <CoreVideo/CoreVideo.h>
  #import <Foundation/Foundation.h>

  // local includes
  #import <src/platform/macos/sc_capture.h>
  #include <src/platform/macos/sc_capture_loop.h>

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Build a small sample buffer that needs no capture permission.
   *
   * @return Retained sample buffer wrapping a blank NV12 pixel buffer.
   */
  CMSampleBufferRef make_sample_buffer() {
    CVPixelBufferRef pixel_buffer = nullptr;
    CVPixelBufferCreate(kCFAllocatorDefault, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pixel_buffer);

    CMVideoFormatDescriptionRef format_desc = nullptr;
    CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel_buffer, &format_desc);

    CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
    CMSampleBufferRef sample_buffer = nullptr;
    CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixel_buffer, YES, nullptr, nullptr, format_desc, &timing, &sample_buffer);

    CFRelease(format_desc);
    CVPixelBufferRelease(pixel_buffer);
    return sample_buffer;
  }

  /**
   * @brief Image type handed out by the fake pipeline.
   */
  struct test_img_t: platf::img_t {};

  /**
   * @brief Scripted stand-in for an SCStream.
   *
   * Each wait consumes the next event: a frame delivers a sample buffer, a timeout delivers
   * nothing, and a stop makes the source report an unexpected stream stop.
   */
  class fake_frame_source_t: public platf::sckit::frame_source_t {
  public:
    /**
     * @brief Scripted stream events.
     */
    enum class event_e {
      timeout,  ///< No frame arrived within the poll interval.
      frame,  ///< A frame is available.
      stop  ///< The stream stopped on its own.
    };

    bool start_result = true;  ///< Result returned by start().
    std::deque<event_e> events;  ///< Events replayed in order.
    int start_calls = 0;  ///< Number of start() calls.
    int stop_calls = 0;  ///< Number of stop() calls.
    int waits = 0;  ///< Number of wait_frame() calls.

    bool start() override {
      ++start_calls;
      return start_result;
    }

    void wait_frame(std::chrono::nanoseconds) override {
      ++waits;
    }

    [[nodiscard]] bool stopped() const override {
      return !events.empty() && events.front() == event_e::stop;
    }

    CMSampleBufferRef copy_latest_sample_buffer() override {
      if (events.empty()) {
        return nullptr;
      }
      const auto event = events.front();
      events.pop_front();
      return event == event_e::frame ? make_sample_buffer() : nullptr;
    }

    void stop() override {
      ++stop_calls;
    }
  };

  /**
   * @brief Fake pipeline recording what the loop pushes and controlling when it interrupts.
   */
  struct fake_pipeline_t {
    bool pull_ok = true;  ///< Whether pull_free_image_cb hands out an image.
    bool process_ok = true;  ///< Whether process_frame accepts the sample buffer.
    int push_limit = 1000;  ///< push_captured_image_cb interrupts once this many pushes happened.
    int processed = 0;  ///< Number of process_frame calls.
    std::vector<bool> pushed;  ///< frame_captured flag of every push, in order.

    /**
     * @brief Run the loop against this pipeline.
     *
     * @param source Scripted stream.
     * @param policy Backend policy under test.
     * @return Loop status.
     */
    platf::capture_e run(fake_frame_source_t &source, platf::sckit::backend_policy_t &policy) {
      return platf::sckit::run_capture_loop(
        source,
        policy,
        [this](std::shared_ptr<platf::img_t> &&, bool frame_captured) {
          pushed.push_back(frame_captured);
          return static_cast<int>(pushed.size()) < push_limit;
        },
        [this](std::shared_ptr<platf::img_t> &img_out) {
          img_out = std::make_shared<test_img_t>();
          return pull_ok;
        },
        [this](CMSampleBufferRef, platf::img_t *) {
          ++processed;
          return process_ok;
        },
        1ms
      );
    }
  };
}  // namespace

TEST(SCCaptureLoop, StreamStartFailureReturnsReinitAndDisablesSckit) {
  fake_frame_source_t source;
  source.start_result = false;
  platf::sckit::backend_policy_t policy;
  fake_pipeline_t pipeline;

  EXPECT_EQ(pipeline.run(source, policy), platf::capture_e::reinit);
  EXPECT_TRUE(policy.stream_start_failed());
  EXPECT_FALSE(policy.prefer_sckit(true));
  EXPECT_EQ(source.start_calls, 1);
  EXPECT_EQ(source.stop_calls, 0);
  EXPECT_TRUE(pipeline.pushed.empty());
}

TEST(SCCaptureLoop, UnexpectedStreamStopReturnsReinit) {
  using event_e = fake_frame_source_t::event_e;
  fake_frame_source_t source;
  source.events = {event_e::frame, event_e::frame, event_e::stop};
  platf::sckit::backend_policy_t policy;
  fake_pipeline_t pipeline;

  EXPECT_EQ(pipeline.run(source, policy), platf::capture_e::reinit);
  EXPECT_EQ(pipeline.pushed, (std::vector<bool> {true, true}));
  EXPECT_EQ(pipeline.processed, 2);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_FALSE(policy.stream_start_failed());
  EXPECT_TRUE(policy.prefer_sckit(true));
}

TEST(SCCaptureLoop, PollTimeoutReportsRepeatFrame) {
  using event_e = fake_frame_source_t::event_e;
  fake_frame_source_t source;
  source.events = {event_e::timeout, event_e::frame};
  platf::sckit::backend_policy_t policy;
  fake_pipeline_t pipeline;
  pipeline.push_limit = 2;

  EXPECT_EQ(pipeline.run(source, policy), platf::capture_e::ok);
  EXPECT_EQ(pipeline.pushed, (std::vector<bool> {false, true}));
  EXPECT_EQ(pipeline.processed, 1);
  EXPECT_EQ(source.stop_calls, 1);
}

TEST(SCCaptureLoop, PipelineInterruptOnPullReturnsOk) {
  using event_e = fake_frame_source_t::event_e;
  fake_frame_source_t source;
  source.events = {event_e::frame};
  platf::sckit::backend_policy_t policy;
  fake_pipeline_t pipeline;
  pipeline.pull_ok = false;

  EXPECT_EQ(pipeline.run(source, policy), platf::capture_e::ok);
  EXPECT_TRUE(pipeline.pushed.empty());
  EXPECT_EQ(pipeline.processed, 0);
  EXPECT_EQ(source.stop_calls, 1);
}

TEST(SCCaptureLoop, UnusableFrameIsSkipped) {
  using event_e = fake_frame_source_t::event_e;
  fake_frame_source_t source;
  source.events = {event_e::frame, event_e::stop};
  platf::sckit::backend_policy_t policy;
  fake_pipeline_t pipeline;
  pipeline.process_ok = false;

  EXPECT_EQ(pipeline.run(source, policy), platf::capture_e::reinit);
  EXPECT_EQ(pipeline.processed, 1);
  EXPECT_TRUE(pipeline.pushed.empty());
  EXPECT_EQ(source.stop_calls, 1);
}

TEST(SCCaptureLoop, BackendPolicyPrefersSckitUntilAStreamFailsToStart) {
  platf::sckit::backend_policy_t policy;

  EXPECT_FALSE(policy.stream_start_failed());
  EXPECT_TRUE(policy.prefer_sckit(true));
  EXPECT_FALSE(policy.prefer_sckit(false));

  policy.note_stream_start_failure();

  EXPECT_TRUE(policy.stream_start_failed());
  EXPECT_FALSE(policy.prefer_sckit(true));
  EXPECT_FALSE(policy.prefer_sckit(false));
}

TEST(SCCaptureLoop, StopDelegateMarksStreamFailedUnlessStopping) {
  if (@available(macOS 14.0, *)) {
    SCCapture *capture = [[SCCapture alloc] init];
    ASSERT_NE(capture, nil);

    SCStream *stream = nil;
    NSError *error = [NSError errorWithDomain:@"test" code:1 userInfo:nil];
    [capture stream:stream didStopWithError:error];
    EXPECT_TRUE(capture.streamFailed);

    // A stop requested by the backend itself must not look like a failure.
    capture.streamFailed = NO;
    [capture stopCapture];
    [capture stream:stream didStopWithError:error];
    EXPECT_FALSE(capture.streamFailed);

    [capture release];
  }
}
#endif
