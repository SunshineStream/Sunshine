/**
 * @file tests/unit/test_legacy_codec_roundtrip.cpp
 * @brief Round-trip negotiated legacy codecs through production encoding and client-side decoding.
 */
// test includes
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

namespace video {
  /**
   * @brief Create the production conversion device without starting desktop capture.
   * @param display Deterministic display supplying CPU frames.
   * @param encoder Backend under test.
   * @param config Negotiated client configuration.
   * @return Production encode device.
   */
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &display, const encoder_t &encoder, const config_t &config);

  /**
   * @brief Create a production encoding session for deterministic test frames.
   * @param display Display geometry and codec support.
   * @param encoder Backend under test.
   * @param config Negotiated client configuration.
   * @param width Capture width.
   * @param height Capture height.
   * @param device Frame conversion device.
   * @return Production encode session.
   */
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *display, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> device);

  /**
   * @brief Encode and queue a frame using the same path as a streaming session.
   * @param frame_nr Monotonic frame number.
   * @param session Production encode session.
   * @param packets Encoded packet queue.
   * @param channel_data Unused stream identity for this headless test.
   * @param frame_timestamp Optional capture timestamp.
   * @return Zero on successful encoding.
   */
  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp);
}  // namespace video

namespace {
  /**
   * @brief CPU display exercising production color conversion without a desktop or GPU.
   */
  class test_display_t: public platf::display_t {
  public:
    /**
     * @brief Set a small frame size accepted by both legacy codecs.
     */
    test_display_t() {
      width = env_width = 176;
      height = env_height = 144;
    }

    /**
     * @brief Keep desktop capture disabled; frames are supplied directly by the test.
     * @param push Unused capture callback.
     * @param pull Unused image allocation callback.
     * @param cursor Unused cursor state.
     * @return Successful capture shutdown.
     */
    platf::capture_e capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &pull, bool *cursor) override {
      return platf::capture_e::ok;
    }

    /**
     * @brief Leave image allocation to the deterministic test buffer.
     * @return Empty image handle.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      return {};
    }

    /**
     * @brief Accept the caller's initialized image.
     * @param image Unused image.
     * @return Zero on success.
     */
    int dummy_img(platf::img_t *image) override {
      return 0;
    }

    /**
     * @brief Request production software color conversion.
     * @param format Requested encoder pixel format.
     * @return CPU encode device wrapped by the production software converter.
     */
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e format) override {
      return std::make_unique<platf::avcodec_encode_device_t>();
    }
  };

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
  video::config_t config {};
  config.videoFormat = getVideoBitStreamFormat(negotiated);
  config.width = 176;
  config.height = 144;
  config.framerate = 30;
  config.framerateX100 = 3000;
  config.bitrate = 1000;
  config.slicesPerFrame = 1;
  config.numRefFrames = 1;
  config.encoderCscMode = 1;

  auto &codec = config.videoFormat == video::format_mpeg2 ? video::software.mpeg2 : video::software.h263p;
  const auto saved_capabilities = codec.capabilities;
  auto restore = util::fail_guard([&] {
    codec.capabilities = saved_capabilities;
  });
  codec[video::encoder_t::PASSED] = true;

  test_display_t display;
  auto device = video::make_encode_device(display, video::software, config);
  ASSERT_TRUE(device);
  auto session = video::make_encode_session(&display, video::software, config, display.width, display.height, std::move(device));
  ASSERT_TRUE(session);

  const auto decoder = avcodec_find_decoder(config.videoFormat == video::format_mpeg2 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_H263P);
  ASSERT_NE(decoder, nullptr);
  video::avcodec_ctx_t decode_context {avcodec_alloc_context3(decoder)};
  ASSERT_TRUE(decode_context);
  decode_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ASSERT_EQ(avcodec_open2(decode_context.get(), decoder, nullptr), 0);
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
  std::vector<uint8_t> pixels(static_cast<std::size_t>(config.width) * config.height * 4, 64);
  platf::img_t image;
  image.data = pixels.data();
  image.width = config.width;
  image.height = config.height;
  image.pixel_pitch = 4;
  image.row_pitch = config.width * image.pixel_pitch;
  int encoded_frames = 0;
  bool saw_inter_frame = false;
  for (int frame = 0; frame < 5; ++frame) {
    pixels[0] = static_cast<uint8_t>(frame * 20);
    ASSERT_EQ(session->convert(image), 0);
    if (frame == 0) {
      session->request_idr_frame();
    } else {
      session->request_normal_frame();
    }
    ASSERT_EQ(video::encode(frame, *session, packets, nullptr, std::nullopt), 0);
    while (packets->peek()) {
      auto packet = packets->pop();
      ASSERT_TRUE(packet);
      const auto &encoded = packet;
      ASSERT_GT(encoded->data_size(), 0);
      if (encoded_frames++ == 0) {
        EXPECT_TRUE(encoded->is_idr());
      } else {
        saw_inter_frame |= !encoded->is_idr();
      }
      auto *av_packet = av_packet_alloc();
      ASSERT_NE(av_packet, nullptr);
      auto release = util::fail_guard([&] {
        av_packet_free(&av_packet);
      });
      ASSERT_EQ(av_new_packet(av_packet, static_cast<int>(encoded->data_size())), 0);
      std::copy_n(encoded->data(), encoded->data_size(), av_packet->data);
      ASSERT_EQ(avcodec_send_packet(decode_context.get(), av_packet), 0);
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
