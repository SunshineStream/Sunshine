/**
 * @file tests/unit/test_legacy_codec_roundtrip.cpp
 * @brief Round-trip negotiated legacy codecs through production encoding and client-side decoding.
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

// lib includes
extern "C" {
#include <moonlight-common-c/src/VideoFormats.h>
}

// local includes
#include <src/rtsp.h>
#include <src/video.h>

namespace {
  /**
   * @brief Run the same codec transport contract for MPEG-2 and H.263+.
   */
  class LegacyCodecRoundTripTest: public BaseTest, public testing::WithParamInterface<int> {};
}  // namespace

/**
 * @brief Negotiate a forced codec, encode BGRA frames, and decode the resulting key and inter frames.
 */
TEST_P(LegacyCodecRoundTripTest, DecodesProductionPacketsForTheNegotiatedCodec) {
  const auto format = GetParam();
  const auto mode = format == VIDEO_FORMAT_MPEG2 ? SCM_MPEG2 : SCM_H263P;
  const auto sdp = rtsp_stream::describe_video_codecs(mode);
  const auto negotiated = chooseVideoFormat(format, mode, sdp.c_str());
  ASSERT_EQ(negotiated, format);
  const auto config = test_utils::software_encode::small_stream_config(getVideoBitStreamFormat(negotiated));

  auto &codec = config.videoFormat == video::format_mpeg2 ? video::software.mpeg2 : video::software.h263p;
  const auto saved_capabilities = codec.capabilities;
  auto restore = util::fail_guard([&codec, &saved_capabilities] {
    codec.capabilities = saved_capabilities;
  });
  codec[video::encoder_t::PASSED] = true;

  test_utils::software_encode::cpu_display_t display;
  auto session = test_utils::software_encode::make_software_session(display, config);
  ASSERT_TRUE(session);

  auto decode_context = test_utils::software_encode::open_low_delay_decoder(config.videoFormat == video::format_mpeg2 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_H263P);
  ASSERT_TRUE(decode_context);
  video::avcodec_frame_t decoded {av_frame_alloc()};
  ASSERT_TRUE(decoded);
  int decoded_frames = 0;
  const auto receive = [&] {
    int result;
    while ((result = avcodec_receive_frame(decode_context.get(), decoded.get())) == 0) {
      EXPECT_EQ(decoded->width, config.width);
      EXPECT_EQ(decoded->height, config.height);
      EXPECT_EQ(decoded->format, AV_PIX_FMT_YUV420P);
      ++decoded_frames;
      av_frame_unref(decoded.get());
    }
    EXPECT_TRUE(result == AVERROR(EAGAIN) || result == AVERROR_EOF) << result;
  };

  auto mailbox = std::make_shared<safe::mail_raw_t>();
  auto packets = mailbox->queue<video::packet_t>("video");
  test_utils::software_encode::bgra_frame_t bgra {config.width, config.height, 64};
  int encoded_frames = 0;
  bool saw_inter_frame = false;
  for (int frame = 0; frame < 5; ++frame) {
    ASSERT_EQ(test_utils::software_encode::encode_test_frame(*session, bgra, frame, packets), 0);
    while (packets->peek()) {
      auto packet = packets->pop();
      ASSERT_TRUE(packet);
      ASSERT_GT(packet->data_size(), 0);
      if (encoded_frames++ == 0) {
        EXPECT_TRUE(packet->is_idr());
      } else {
        saw_inter_frame |= !packet->is_idr();
      }
      ASSERT_EQ(test_utils::software_encode::send_packet_to_decoder(decode_context.get(), *packet), 0);
      receive();
    }
  }
  ASSERT_EQ(avcodec_send_packet(decode_context.get(), nullptr), 0);
  receive();
  EXPECT_GE(encoded_frames, 2);
  EXPECT_TRUE(saw_inter_frame);
  EXPECT_EQ(decoded_frames, encoded_frames);
}

INSTANTIATE_TEST_SUITE_P(LegacyFormats, LegacyCodecRoundTripTest, testing::Values(VIDEO_FORMAT_MPEG2, VIDEO_FORMAT_H263P));
