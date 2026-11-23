/**
 * @file tests/software_encode_test_utils.h
 * @brief Helpers shared by tests that run the production software encode path on CPU frames.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// local includes
#include <src/platform/common.h>
#include <src/utility.h>
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

namespace test_utils::software_encode {
  /**
   * @brief CPU display exercising production color conversion without a desktop or GPU.
   */
  class cpu_display_t: public platf::display_t {
  public:
    /**
     * @brief Set a small frame size accepted by every software codec, including the legacy ones.
     */
    cpu_display_t() {
      width = 176;
      height = 144;
      env_width = width;
      env_height = height;
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
   * @brief Stream configuration for a small, single-slice software encode at 30 fps and 1000 kbps.
   *
   * @param video_format Sunshine video format to encode.
   * @return Configuration matching `cpu_display_t`.
   */
  inline video::config_t small_stream_config(int video_format) {
    video::config_t config {};
    config.videoFormat = video_format;
    config.width = 176;
    config.height = 144;
    config.framerate = 30;
    config.framerateX100 = 3000;
    config.bitrate = 1000;
    config.slicesPerFrame = 1;
    config.numRefFrames = 1;
    config.encoderCscMode = 1;
    return config;
  }

  /**
   * @brief Create a production software encode session for a CPU display.
   *
   * @param display Display supplying the frame geometry.
   * @param config Negotiated client configuration.
   * @return Encode session, or null on failure.
   */
  inline std::unique_ptr<video::encode_session_t> make_software_session(cpu_display_t &display, const video::config_t &config) {
    auto device = video::make_encode_device(display, video::software, config);
    if (!device) {
      return nullptr;
    }
    return video::make_encode_session(&display, video::software, config, display.width, display.height, std::move(device));
  }

  /**
   * @brief Open a low-delay FFmpeg decoder.
   *
   * @param codec_id Codec to decode.
   * @return Open decoder context, or an empty context on failure.
   */
  inline video::avcodec_ctx_t open_low_delay_decoder(AVCodecID codec_id) {
    const auto decoder = avcodec_find_decoder(codec_id);
    if (!decoder) {
      return {};
    }
    video::avcodec_ctx_t context {avcodec_alloc_context3(decoder)};
    if (!context) {
      return {};
    }
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(context.get(), decoder, nullptr) != 0) {
      return {};
    }
    return context;
  }

  /**
   * @brief BGRA frame owning its pixels, ready to pass to an encode session's `convert`.
   */
  class bgra_frame_t {
  public:
    /**
     * @brief Allocate a uniformly filled frame.
     * @param width Frame width in pixels.
     * @param height Frame height in pixels.
     * @param fill Value of every byte.
     */
    bgra_frame_t(int width, int height, std::uint8_t fill):
        pixels(static_cast<std::size_t>(width) * height * 4, fill) {
      image.data = pixels.data();
      image.width = width;
      image.height = height;
      image.pixel_pitch = 4;
      image.row_pitch = width * image.pixel_pitch;
    }

    /**
     * @brief Not copyable, because `image` points into `pixels`.
     */
    bgra_frame_t(const bgra_frame_t &) = delete;

    /**
     * @brief Not copyable, because `image` points into `pixels`.
     * @return Never returns.
     */
    bgra_frame_t &operator=(const bgra_frame_t &) = delete;

    std::vector<std::uint8_t> pixels;  ///< BGRA pixel bytes.
    platf::img_t image;  ///< Image view over `pixels`.
  };

  /**
   * @brief Encode one test frame: the first frame is requested as IDR, the others as inter frames.
   *
   * The first pixel changes with the frame number so consecutive frames differ.
   *
   * @param session Production encode session.
   * @param frame Frame to encode.
   * @param frame_nr Monotonic frame number, starting at 0.
   * @param packets Queue receiving the encoded packets.
   * @return Zero on success.
   */
  inline int encode_test_frame(video::encode_session_t &session, bgra_frame_t &frame, int frame_nr, safe::mail_raw_t::queue_t<video::packet_t> &packets) {
    frame.pixels[0] = static_cast<std::uint8_t>(frame_nr * 20);
    if (auto status = session.convert(frame.image)) {
      return status;
    }
    if (frame_nr == 0) {
      session.request_idr_frame();
    } else {
      session.request_normal_frame();
    }
    return video::encode(frame_nr, session, packets, nullptr, std::nullopt);
  }

  /**
   * @brief Send an encoded packet to an FFmpeg decoder.
   *
   * @param decoder Open decoder context.
   * @param packet Encoded packet produced by an encode session.
   * @return Result of `avcodec_send_packet`, or a negative FFmpeg error if the packet could not be allocated.
   */
  inline int send_packet_to_decoder(AVCodecContext *decoder, video::packet_raw_t &packet) {
    auto *av_packet = av_packet_alloc();
    if (!av_packet) {
      return AVERROR(ENOMEM);
    }
    auto release = util::fail_guard([&av_packet] {
      av_packet_free(&av_packet);
    });
    if (auto status = av_new_packet(av_packet, static_cast<int>(packet.data_size()))) {
      return status;
    }
    std::copy_n(packet.data(), packet.data_size(), av_packet->data);
    return avcodec_send_packet(decoder, av_packet);
  }
}  // namespace test_utils::software_encode
