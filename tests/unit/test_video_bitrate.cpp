/**
 * @file tests/unit/test_video_bitrate.cpp
 * @brief Tests for runtime bitrate changes of encode sessions.
 */
// test includes
#include "../software_encode_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

// local includes
#include <src/config.h>
#include <src/utility.h>
#include <src/video.h>

namespace video {
  /**
   * @brief Apply a pending bitrate change to an encode session.
   * @param session Encode session that receives the change.
   * @param bitrate_change_events Event carrying new target bitrates, in kbps.
   * @param bitrate_kbps Last target the session accepted.
   */
  void apply_bitrate_changes(encode_session_t &session, const safe::mail_raw_t::event_t<int> &bitrate_change_events, std::optional<int> &bitrate_kbps);

  /**
   * @brief Carry an adaptive bitrate over to a newly created encode session.
   * @param session Newly created encode session.
   * @param bitrate_kbps Last target accepted by the previous encode session, if any.
   */
  void restore_bitrate(encode_session_t &session, const std::optional<int> &bitrate_kbps);
}  // namespace video

namespace {
  /**
   * @brief Encode session that records the bitrate changes it receives.
   */
  class recording_session_t: public video::encode_session_t {
  public:
    /**
     * @brief Create a session that accepts or rejects bitrate changes.
     * @param accept Whether `set_bitrate` succeeds.
     */
    explicit recording_session_t(bool accept):
        accept {accept} {
    }

    /**
     * @brief Ignore the image.
     * @param img Unused image.
     * @return Zero.
     */
    int convert(platf::img_t &img) override {
      return 0;
    }

    /**
     * @brief Ignore the request.
     */
    void request_idr_frame() override {
      // Not used by these tests: only bitrate changes are recorded.
    }

    /**
     * @brief Ignore the request.
     */
    void request_normal_frame() override {
      // Not used by these tests: only bitrate changes are recorded.
    }

    /**
     * @brief Ignore the request.
     * @param first_frame Unused first frame.
     * @param last_frame Unused last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      // Not used by these tests: only bitrate changes are recorded.
    }

    /**
     * @brief Record the requested bitrate.
     * @param bitrate_kbps Requested bitrate, in kbps.
     * @return Whether the session accepts changes.
     */
    bool set_bitrate(int bitrate_kbps) override {
      requested.push_back(bitrate_kbps);
      return accept;
    }

    bool accept;  ///< Whether `set_bitrate` succeeds.
    std::vector<int> requested;  ///< Every bitrate requested, in order.
  };

}  // namespace

/**
 * @brief A pending change reaches the session and is remembered once accepted.
 */
TEST(VideoBitrateChangeTest, RemembersAcceptedChanges) {
  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto events = mailbox->event<int>("bitrate_change");
  recording_session_t session {true};
  std::optional<int> bitrate_kbps;

  video::apply_bitrate_changes(session, events, bitrate_kbps);
  EXPECT_TRUE(session.requested.empty());
  EXPECT_FALSE(bitrate_kbps);

  events->raise(9000);
  events->raise(7000);
  video::apply_bitrate_changes(session, events, bitrate_kbps);
  EXPECT_EQ(session.requested, (std::vector {7000}));
  EXPECT_EQ(bitrate_kbps, 7000);
  EXPECT_FALSE(events->peek());
}

/**
 * @brief A change the encoder does not support is not remembered.
 */
TEST(VideoBitrateChangeTest, ForgetsRejectedChanges) {
  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto events = mailbox->event<int>("bitrate_change");
  recording_session_t session {false};
  std::optional<int> bitrate_kbps;

  events->raise(7000);
  video::apply_bitrate_changes(session, events, bitrate_kbps);
  EXPECT_EQ(session.requested, (std::vector {7000}));
  EXPECT_FALSE(bitrate_kbps);
}

/**
 * @brief A new encode session gets the last accepted bitrate, and nothing before any change.
 */
TEST(VideoBitrateChangeTest, RestoresTheLastBitrateOnANewSession) {
  recording_session_t fresh {true};
  video::restore_bitrate(fresh, std::nullopt);
  EXPECT_TRUE(fresh.requested.empty());

  recording_session_t reinitialized {true};
  video::restore_bitrate(reinitialized, 7000);
  EXPECT_EQ(reinitialized.requested, (std::vector {7000}));
}

/**
 * @brief An FFmpeg encode session rebuilds its codec at the new bitrate and restarts with a decodable key frame.
 */
TEST(VideoBitrateChangeTest, AvcodecSessionChangesBitrateWithAKeyFrame) {
  const auto original_max_bitrate = config::video.max_bitrate;
  const auto saved_capabilities = video::software.mpeg2.capabilities;
  auto restore = util::fail_guard([&saved_capabilities, original_max_bitrate] {
    video::software.mpeg2.capabilities = saved_capabilities;
    config::video.max_bitrate = original_max_bitrate;
  });
  video::software.mpeg2[video::encoder_t::PASSED] = true;
  config::video.max_bitrate = 1500;

  const auto config = test_utils::software_encode::small_stream_config(video::format_mpeg2);
  test_utils::software_encode::cpu_display_t display;
  auto session = test_utils::software_encode::make_software_session(display, config);
  ASSERT_TRUE(session);

  auto decode_context = test_utils::software_encode::open_low_delay_decoder(AV_CODEC_ID_MPEG2VIDEO);
  ASSERT_TRUE(decode_context);
  video::avcodec_frame_t decoded {av_frame_alloc()};
  ASSERT_TRUE(decoded);
  int decoded_frames = 0;
  const auto receive = [&] {
    while (avcodec_receive_frame(decode_context.get(), decoded.get()) == 0) {
      ++decoded_frames;
      av_frame_unref(decoded.get());
    }
  };

  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto packets = mailbox->queue<video::packet_t>("video");
  test_utils::software_encode::bgra_frame_t bgra {config.width, config.height, 64};

  std::vector<bool> idr_frames;
  for (int frame = 0; frame < 6; ++frame) {
    if (frame == 3) {
      // Above the host ceiling, so the rebuilt codec is clamped to max_bitrate
      ASSERT_TRUE(session->set_bitrate(5000));
    }
    ASSERT_EQ(test_utils::software_encode::encode_test_frame(*session, bgra, frame, packets), 0);
    while (packets->peek()) {
      auto packet = packets->pop();
      ASSERT_TRUE(packet);
      ASSERT_GT(packet->data_size(), 0);
      idr_frames.push_back(packet->is_idr());
      ASSERT_EQ(test_utils::software_encode::send_packet_to_decoder(decode_context.get(), *packet), 0);
      receive();
    }
  }
  ASSERT_EQ(avcodec_send_packet(decode_context.get(), nullptr), 0);
  receive();

  ASSERT_EQ(idr_frames.size(), 6);
  EXPECT_EQ(idr_frames, (std::vector {true, false, false, true, false, false}));
  EXPECT_EQ(decoded_frames, 6);
}
