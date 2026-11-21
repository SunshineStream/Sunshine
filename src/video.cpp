/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <deque>
#include <limits>
#include <list>
#include <thread>
#include <utility>

// lib includes
#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#if !defined(_WIN32) && !defined(__APPLE__)
  #include <ffnvcodec/nvEncodeAPI.h>
#endif
}

// local includes
#include "cbs.h"
#include "config.h"
#include "display_device.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_encoder.h"
#include "platform/common.h"
#include "sync.h"
#include "video.h"

#ifdef _WIN32
extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}
#endif

#ifdef SUNSHINE_BUILD_VULKAN
extern "C" {
  #include <libavutil/hwcontext_vulkan.h>
}
#endif

using namespace std::literals;

namespace video {

  bool allow_encoder_probing(const std::vector<display_device::EnumeratedDevice> &devices) {
    // If there are no devices, then either the API is not working correctly or OS does not support the lib.
    // Either way we should not block the probing in this case as we can't tell what's wrong.
    if (devices.empty()) {
      return true;
    }

#ifdef _WIN32
    // Since Windows 11 24H2, it is possible that there will be no active devices present
    // for some reason (probably a bug). Trying to probe encoders in such a state locks/breaks the DXGI
    // and also the display device for Windows. So we must have at least 1 active device.
    const bool at_least_one_device_is_active = std::ranges::any_of(devices, [](const auto &device) {
      // If device has additional info, it is active.
      return static_cast<bool>(device.m_info);
    });

    if (at_least_one_device_is_active) {
      return true;
    }

    BOOST_LOG(error) << "No display devices are active at the moment! Cannot probe the encoders.";
    return false;
#else
    // A sleeping display enumerates as inactive on other platforms, and the wake-up call
    // lives in platf::display(), which probing must reach for the display to ever wake.
    return true;
#endif
  }

  /**
   * @brief Release context resources.
   */
  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   */
  void free_buffer(AVBufferRef *ref) {
    av_buffer_unref(&ref);
  }

  namespace nv {

    /**
     * @brief Enumerates supported profile h264 options.
     */
    enum class profile_h264_e : int {
      high = 2,  ///< High profile
      high_444p = 3,  ///< High 4:4:4 Predictive profile
    };

    /**
     * @brief Enumerates supported profile HEVC options.
     */
    enum class profile_hevc_e : int {
      main = 0,  ///< Main profile
      main_10 = 1,  ///< Main 10 profile
      rext = 2,  ///< Rext profile
    };

  }  // namespace nv

  namespace qsv {

    /**
     * @brief Enumerates supported profile h264 options.
     */
    enum class profile_h264_e : int {
      high = 100,  ///< High profile
      high_444p = 244,  ///< High 4:4:4 Predictive profile
    };

    /**
     * @brief Enumerates supported profile HEVC options.
     */
    enum class profile_hevc_e : int {
      main = 1,  ///< Main profile
      main_10 = 2,  ///< Main 10 profile
      rext = 4,  ///< RExt profile
    };

    /**
     * @brief Enumerates supported profile AV1 options.
     */
    enum class profile_av1_e : int {
      main = 1,  ///< Main profile
      high = 2,  ///< High profile
    };

  }  // namespace qsv

  int select_h264_profile(std::string_view encoder_name, const config_t &config, int amd_coder) {
    if (config.chromaSamplingType == 1) {
      return AV_PROFILE_H264_HIGH_444_PREDICTIVE;
    }

    if (encoder_name == "h264_amf"sv && amd_coder == std::to_underlying(amf::coder_e::cavlc)) {
      return AV_PROFILE_H264_CONSTRAINED_BASELINE;
    }

    return AV_PROFILE_H264_HIGH;
  }

  /**
   * @brief Resolve a client-requested dynamic range against probed encoder capabilities.
   *
   * @param encoder Selected encoder and its probed codec capabilities.
   * @param config Client-requested stream configuration.
   * @return Effective stream configuration, downgraded to SDR when HDR is unsupported.
   */
  config_t resolve_dynamic_range(const encoder_t &encoder, config_t config) {
    // PyroWave profiles are independently probed and validated during ANNOUNCE.
    if (config.videoFormat == pyrowave::video_format) {
      return config;
    }
    if (!config.dynamicRange) {
      return config;
    }

    const auto &video_format = encoder.codec_from_config(config);
    const auto capability = config.chromaSamplingType == 1 ?
                              encoder_t::DYNAMIC_RANGE_YUV444 :
                              encoder_t::DYNAMIC_RANGE;
    if (video_format[capability]) {
      return config;
    }

    const auto mode = config.chromaSamplingType == 1 ? "YUV 4:4:4 dynamic range"sv : "dynamic range"sv;
    BOOST_LOG(warning) << video_format.name << ": "sv << mode << " not supported, falling back to SDR"sv;
    config.dynamicRange = 0;
    return config;
  }

  /**
   * @brief Create an FFmpeg hardware device buffer for D3D11VA input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for VA-API input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for CUDA input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for Vulkan input.
   *
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);

  int avcodec_software_encode_device_t::convert(platf::img_t &img) {
    // If we need to add aspect ratio padding, we need to scale into an intermediate output buffer
    bool requires_padding = (sw_frame->width != sws_output_frame->width || sw_frame->height != sws_output_frame->height);

    // Detect the actual capture pixel format. PipeWire-based captures (KWin
    // screencast / XDG portal) deliver NV12 with 1 byte per pixel, while
    // KMS/DMABUF captures deliver BGR0 (4 bytes per pixel). The capture
    // backend reports bytes per pixel in img.pixel_pitch; fall back to the row
    // pitch heuristic when it is unavailable.
    const auto pixel_pitch = img.pixel_pitch > 0 ? img.pixel_pitch : (img.row_pitch / std::max(img.width, 1));
    const auto input_fmt = (pixel_pitch == 1) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_BGR0;

    // The sws context is created with the default BGR0 source format;
    // recreate it once if the capture is actually NV12.
    if (input_fmt != sws_src_format) {
      sws_src_format = input_fmt;
      if (reinit_sws(input_fmt) < 0) {
        return -1;
      }
      // The colorspace details were applied to the previous sws context.
      apply_colorspace();
    }

    // Setup the input frame using the caller's img_t
    sws_input_frame->data[0] = img.data;
    sws_input_frame->linesize[0] = img.row_pitch;
    if (input_fmt == AV_PIX_FMT_NV12) {
      sws_input_frame->data[1] = img.data + static_cast<std::size_t>(img.row_pitch) * img.height;
      sws_input_frame->linesize[1] = img.row_pitch;
    } else {
      sws_input_frame->data[1] = nullptr;
      sws_input_frame->linesize[1] = 0;
    }
    sws_input_frame->data[2] = nullptr;
    sws_input_frame->linesize[2] = 0;
    sws_input_frame->data[3] = nullptr;
    sws_input_frame->linesize[3] = 0;

    // Perform color conversion and scaling to the final size
    auto status = sws_scale_frame(sws.get(), requires_padding ? sws_output_frame.get() : sw_frame.get(), sws_input_frame.get());
    if (status < 0) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> string;
      BOOST_LOG(error) << "Couldn't scale frame: "sv << av_make_error_string(string.data(), AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    // If we require aspect ratio padding, copy the output frame into the final padded frame
    if (requires_padding) {
      auto fmt_desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(sws_output_frame->format));
      auto planes = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(sws_output_frame->format));
      for (int plane = 0; plane < planes; plane++) {
        auto shift_h = plane == 0 ? 0 : fmt_desc->log2_chroma_h;
        auto shift_w = plane == 0 ? 0 : fmt_desc->log2_chroma_w;
        auto offset = ((offsetW >> shift_w) * fmt_desc->comp[plane].step) + (offsetH >> shift_h) * sw_frame->linesize[plane];

        // Copy line-by-line to preserve leading padding for each row
        for (int line = 0; line < sws_output_frame->height >> shift_h; line++) {
          memcpy(sw_frame->data[plane] + offset + (line * sw_frame->linesize[plane]), sws_output_frame->data[plane] + (line * sws_output_frame->linesize[plane]), static_cast<std::size_t>(sws_output_frame->width >> shift_w) * fmt_desc->comp[plane].step);
        }
      }
    }

    // If frame is not a software frame, it means we still need to transfer from main memory
    // to vram memory
    if (frame->hw_frames_ctx) {
      auto transfer_status = av_hwframe_transfer_data(frame, sw_frame.get(), 0);
      if (transfer_status < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> string;
        BOOST_LOG(error) << "Failed to transfer image data to hardware frame: "sv << av_make_error_string(string.data(), AV_ERROR_MAX_STRING_SIZE, transfer_status);
        return -1;
      }
    }

    return 0;
  }

  int avcodec_software_encode_device_t::set_frame(AVFrame *in_frame, AVBufferRef *hw_frames_ctx) {
    this->frame = in_frame;

    // If it's a hwframe, allocate buffers for hardware
    if (hw_frames_ctx) {
      hw_frame.reset(in_frame);

      if (av_hwframe_get_buffer(hw_frames_ctx, in_frame, 0)) {
        return -1;
      }
    } else {
      sw_frame.reset(in_frame);
    }

    return 0;
  }

  void avcodec_software_encode_device_t::apply_colorspace() {
    auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 0, sws_getCoefficients(avcodec_colorspace.software_format), avcodec_colorspace.range - 1, 0, 1 << 16, 1 << 16);
  }

  void avcodec_software_encode_device_t::prefill() {
    auto active_frame = sw_frame ? sw_frame.get() : this->frame;
    av_frame_get_buffer(active_frame, 0);
    av_frame_make_writable(active_frame);
    std::array<ptrdiff_t, 4> linesize = {active_frame->linesize[0], active_frame->linesize[1], active_frame->linesize[2], active_frame->linesize[3]};
    av_image_fill_black(active_frame->data, linesize.data(), static_cast<AVPixelFormat>(active_frame->format), active_frame->color_range, active_frame->width, active_frame->height);
  }

  int avcodec_software_encode_device_t::init(int in_width, int in_height, AVFrame *in_frame, AVPixelFormat format, bool hardware_frame) {
    // If the device used is hardware, yet the image resides on main memory
    if (hardware_frame) {
      sw_frame.reset(av_frame_alloc());

      sw_frame->width = in_frame->width;
      sw_frame->height = in_frame->height;
      sw_frame->format = format;
    } else {
      this->frame = in_frame;
    }

    // Fill aspect ratio padding in the destination frame
    prefill();

    auto out_width = in_frame->width;
    auto out_height = in_frame->height;

    // Ensure aspect ratio is maintained
    auto scalar = std::fminf(static_cast<float>(out_width) / in_width, static_cast<float>(out_height) / in_height);
    out_width = in_width * scalar;
    out_height = in_height * scalar;

    sws_input_frame.reset(av_frame_alloc());
    sws_input_frame->width = in_width;
    sws_input_frame->height = in_height;
    sws_input_frame->format = AV_PIX_FMT_BGR0;

    sws_output_frame.reset(av_frame_alloc());
    sws_output_frame->width = out_width;
    sws_output_frame->height = out_height;
    sws_output_frame->format = format;

    // Result is always positive
    offsetW = (in_frame->width - out_width) / 2;
    offsetH = (in_frame->height - out_height) / 2;

    sws_src_format = AV_PIX_FMT_BGR0;

    return reinit_sws(sws_src_format);
  }

  int avcodec_software_encode_device_t::reinit_sws(AVPixelFormat src_format) {
    sws_input_frame->format = src_format;

    sws.reset(sws_alloc_context());
    if (!sws) {
      return -1;
    }

    AVDictionary *options {nullptr};
    av_dict_set_int(&options, "srcw", sws_input_frame->width, 0);
    av_dict_set_int(&options, "srch", sws_input_frame->height, 0);
    av_dict_set_int(&options, "src_format", src_format, 0);
    av_dict_set_int(&options, "dstw", sws_output_frame->width, 0);
    av_dict_set_int(&options, "dsth", sws_output_frame->height, 0);
    av_dict_set_int(&options, "dst_format", sws_output_frame->format, 0);
    av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
    av_dict_set_int(&options, "threads", config::video.min_threads, 0);

    auto status = av_opt_set_dict(sws.get(), &options);
    av_dict_free(&options);
    if (status < 0) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> string;
      BOOST_LOG(error) << "Failed to set SWS options: "sv << av_make_error_string(string.data(), AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    status = sws_init_context(sws.get(), nullptr, nullptr);
    if (status < 0) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> string;
      BOOST_LOG(error) << "Failed to initialize SWS: "sv << av_make_error_string(string.data(), AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return 0;
  }

  /**
   * @brief Enumerates supported flag options.
   */
  enum flag_e : uint32_t {
    DEFAULT = 0,  ///< Default flags
    PARALLEL_ENCODING = 1 << 1,  ///< Capture and encoding can run concurrently on separate threads
    H264_ONLY = 1 << 2,  ///< When HEVC is too heavy
    LIMITED_GOP_SIZE = 1 << 3,  ///< Some encoders don't like it when you have an infinite GOP_SIZE. e.g. VAAPI
    SINGLE_SLICE_ONLY = 1 << 4,  ///< Never use multiple slices. Older intel iGPU's ruin it for everyone else
    CBR_WITH_VBR = 1 << 5,  ///< Use a VBR rate control mode to simulate CBR
    RELAXED_COMPLIANCE = 1 << 6,  ///< Use FF_COMPLIANCE_UNOFFICIAL compliance mode
    NO_RC_BUF_LIMIT = 1 << 7,  ///< Don't set rc_buffer_size
    REF_FRAMES_INVALIDATION = 1 << 8,  ///< Support reference frames invalidation
    ALWAYS_REPROBE = 1 << 9,  ///< This is an encoder of last resort and we want to aggressively probe for a better one
    YUV444_SUPPORT = 1 << 10,  ///< Encoder may support 4:4:4 chroma sampling depending on hardware
    ASYNC_TEARDOWN = 1 << 11,  ///< Encoder supports async teardown on a different thread
    FIXED_GOP_SIZE = 1 << 12,  ///< Use fixed small GOP size (encoder doesn't support on-demand IDR frames)
  };

  /**
   * @brief FFmpeg AVCodec encode session and parameter-set rewriting state.
   */
  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;

    /**
     * @brief Initialize an FFmpeg encode session and its hardware encode device.
     *
     * @param avcodec_ctx Open FFmpeg codec context for the selected encoder.
     * @param encode_device Platform encode device that supplies frames to FFmpeg.
     * @param inject Whether SPS/VPS replacement data should be injected.
     */
    avcodec_encode_session_t(avcodec_ctx_t &&avcodec_ctx, std::unique_ptr<platf::avcodec_encode_device_t> encode_device, int inject):
        avcodec_ctx {std::move(avcodec_ctx)},
        device {std::move(encode_device)},
        inject {inject} {
    }

    /**
     * @brief Move an FFmpeg encode session without duplicating codec/device ownership.
     *
     * @param other Source object whose state is copied or moved into this object.
     */
    avcodec_encode_session_t(avcodec_encode_session_t &&other) noexcept = default;

    ~avcodec_encode_session_t() {
      // Flush any remaining frames in the encoder if the encoder started up (frame num > 0)
      if (avcodec_ctx->frame_num > 0 && avcodec_send_frame(avcodec_ctx.get(), nullptr) == 0) {
        packet_raw_avcodec pkt;
        while (avcodec_receive_packet(avcodec_ctx.get(), pkt.av_packet) == 0);
      }

      // Order matters here because the context relies on the hwdevice still being valid
      avcodec_ctx.reset();
      device.reset();
    }

    // Ensure objects are destroyed in the correct order
    /**
     * @brief Assign state from another instance while preserving ownership semantics.
     *
     * @param other Source object whose state is copied or moved into this object.
     * @return Reference or value produced by the operator.
     */
    avcodec_encode_session_t &operator=(avcodec_encode_session_t &&other) {
      device = std::move(other.device);
      avcodec_ctx = std::move(other.avcodec_ctx);
      replacements = std::move(other.replacements);
      sps = std::move(other.sps);
      vps = std::move(other.vps);
      frame_timestamps = std::move(other.frame_timestamps);

      inject = other.inject;

      return *this;
    }

    /**
     * @brief Encode one frame with FFmpeg AVCodec and prepare packet replacements.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
      }
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
      }
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      BOOST_LOG(error) << "Encoder doesn't support reference frame invalidation";
      request_idr_frame();
    }

    avcodec_ctx_t avcodec_ctx;  ///< FFmpeg codec context owned by the encode session.
    std::unique_ptr<platf::avcodec_encode_device_t> device;  ///< Platform device used by the FFmpeg hardware encoder.

    std::vector<packet_raw_t::replace_t> replacements;  ///< NAL-unit byte ranges that must be replaced before packet send.

    cbs::nal_t sps;  ///< Original and rewritten sequence parameter set for IDR injection.
    cbs::nal_t vps;  ///< Original and rewritten HEVC video parameter set for IDR injection.

    // inject sps/vps data into idr pictures
    int inject;  ///< Number of upcoming IDR frames that should receive rewritten parameter sets.

    frame_timestamp_queue_t frame_timestamps;  ///< Capture timestamps of in-flight frames keyed by pts, awaiting their encoded packets.
  };

  /**
   * @brief NVENC encode session and device state for hardware encoding.
   */
  class nvenc_encode_session_t: public encode_session_t {
  public:
    /**
     * @brief Explicitly initialize an NVENC encode session and take ownership of its device.
     *
     * @param encode_device Encode device.
     */
    explicit nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    /**
     * @brief Encode one frame with NVENC and return the packet payload.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      force_idr = true;
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      force_idr = false;
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->nvenc) {
        return;
      }

      if (!device->nvenc->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    /**
     * @brief Submit the next frame to NVENC and return the encoded payload.
     *
     * @param frame_index Monotonic frame index assigned by the video pipeline.
     * @return Encoded NVENC frame payload and frame metadata.
     */
    nvenc::nvenc_encoded_frame encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) {
        return {};
      }

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    bool force_idr = false;
  };

  namespace pyrowave {
    size_t frame_budget(const config_t &config) {
      if (config.bitrate <= 0 || config.framerate <= 0 || config.framerateX100 < 0 || config.packetSize <= int(shard_overhead) || config.packetSize > config::PACKETSIZE_MAX) {
        return 0;
      }
      const auto rate = config.framerateX100 ? uint64_t(config.framerateX100) : uint64_t(config.framerate) * 100;
      const auto bitrate = config::video.max_bitrate > 0 ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate;
      const auto budget = uint64_t(bitrate) * 1000 * 100 / (8 * rate);
      if (!budget || budget > max_frame_size) {
        return 0;
      }

      const auto payload_size = size_t(config.packetSize) - shard_overhead;
      auto capacity = std::min(max_frame_size, max_frame_packets * payload_size - frame_header_size);
      // The codec budget already includes the eight-byte sequence and block headers.
      if (!config.pyrowaveRecordFraming) {
        // Each native packet is at least eight bytes and needs a four-byte length, plus the packet count.
        capacity = (capacity - 4) * 2 / 3;
      } else if (payload_size >= 24 && payload_size % 4 == 0) {
        // Padding costs at most one record's size plus four bytes per record (minimum eight bytes).
        // Reserve another payload for the critical/detail boundary: framed size <= 5/2 * budget + payload.
        capacity = (capacity - payload_size) * 2 / 5;
      }
      return std::min(size_t(budget), capacity);
    }

    bool valid_config(const config_t &config) {
      return config.videoFormat == video_format &&
             config.width > 0 && config.width <= 16384 &&
             config.height > 0 && config.height <= 16384 &&
             config.packetSize > int(shard_overhead) && config.packetSize <= config::PACKETSIZE_MAX &&
             (config.dynamicRange == 0 || config.dynamicRange == 1) &&
             (config.chromaSamplingType == 1 || (config.chromaSamplingType == 0 && config.width % 2 == 0 && config.height % 2 == 0)) &&
             frame_budget(config) != 0;
    }

    packet_t frame_packets(std::span<const pyrowave_packet> packets, std::span<const uint8_t> bitstream, const config_t &config, size_t critical_blocks, int64_t frame_index) {
      if (packets.empty() || packets.size() > std::numeric_limits<uint32_t>::max() || config.packetSize <= int(shard_overhead) || config.packetSize > config::PACKETSIZE_MAX) {
        return nullptr;
      }
      std::vector<uint8_t> output;
      std::vector<std::span<const uint8_t>> records;
      if (!config.pyrowaveRecordFraming) {
        output.resize(4);
        AV_WL32(output.data(), packets.size());
      }
      size_t native_size = 0;
      for (const auto &packet : packets) {
        if (!packet.size || packet.size > record_packet_boundary || packet.size % 4 || packet.offset > bitstream.size() || packet.size > bitstream.size() - packet.offset || packet.size > max_frame_size - native_size) {
          return nullptr;
        }
        native_size += packet.size;
        auto bytes = bitstream.subspan(packet.offset, packet.size);
        if (!config.pyrowaveRecordFraming) {
          if (output.size() > max_frame_size - 4 - bytes.size()) {
            return nullptr;
          }
          const auto offset = output.size();
          output.resize(offset + 4);
          AV_WL32(output.data() + offset, bytes.size());
          output.insert(output.end(), bytes.begin(), bytes.end());
          continue;
        }
        while (!bytes.empty()) {
          if (bytes.size() < 8) {
            return nullptr;
          }
          const auto word = AV_RL32(bytes.data());
          // Native sequence and block records both have eight-byte headers.
          const bool sequence = word & 0x80000000u;
          const size_t size = sequence ? 8 : ((word >> 16) & 0xfffu) * 4;
          if (sequence != records.empty() || size < 8 || size > bytes.size()) {
            return nullptr;
          }
          records.push_back(bytes.first(size));
          bytes = bytes.subspan(size);
        }
      }
      if (!config.pyrowaveRecordFraming) {
        return std::make_unique<packet_raw_generic>(std::move(output), frame_index, true);
      }

      const auto payload_size = size_t(config.packetSize) - shard_overhead;
      const bool aligned = payload_size >= 24 && payload_size % 4 == 0;
      output.reserve(native_size);
      std::vector<bool> record_starts;
      auto append = [&](std::span<const uint8_t> bytes) {
        if (bytes.size() > max_frame_size - output.size()) {
          return false;
        }
        const auto position = output.size() + frame_header_size;
        if (aligned) {
          record_starts.resize((position + bytes.size() + payload_size - 1) / payload_size);
          if (output.empty() || position % payload_size == 0) {
            record_starts[position / payload_size] = true;
          }
        }
        output.insert(output.end(), bytes.begin(), bytes.end());
        return true;
      };
      auto pad = [&](size_t size) {
        std::array<uint8_t, 8> padding;
        AV_WL32(padding.data(), 0xffffffffu);
        AV_WL32(padding.data() + 4, (size - 8) / 4);
        if (size > max_frame_size - output.size()) {
          return false;
        }
        append(padding);
        output.resize(output.size() + size - padding.size());
        return true;
      };
      auto remaining = [&]() {
        return payload_size - (output.size() + frame_header_size) % payload_size;
      };

      size_t critical_packets = 0;
      auto detail = records.end();
      if (aligned) {
        detail = std::stable_partition(records.begin() + 1, records.end(), [&](auto record) {
          return (AV_RL32(record.data() + 4) >> 8) < critical_blocks;
        });
        // Put records that must span shards first, preserving recovery for small records.
        auto oversized = [&](auto record) {
          return record.size() > payload_size - 8;
        };
        std::stable_partition(records.begin() + 1, detail, oversized);
        std::stable_partition(detail, records.end(), oversized);
      }
      for (auto record = records.begin(); record != records.end(); ++record) {
        if (aligned) {
          if (record == detail) {
            if (remaining() != payload_size && !pad(remaining())) {
              return nullptr;
            }
            critical_packets = (output.size() + frame_header_size) / payload_size;
          }
          if (record->size() <= payload_size - 8) {
            if ((record->size() > remaining() || remaining() - record->size() == 4) && !pad(remaining())) {
              return nullptr;
            }
          } else if ((output.size() + frame_header_size + record->size()) % payload_size == payload_size - 4 && !pad(8)) {
            return nullptr;
          }
        }
        if (!append(*record)) {
          return nullptr;
        }
      }
      if (aligned && detail == records.end()) {
        critical_packets = record_starts.size();
      }
      if (record_starts.size() > max_frame_packets) {
        return nullptr;
      }
      auto result = std::make_unique<packet_raw_generic>(std::move(output), frame_index, true);
      result->pyrowave_critical_packets = uint16_t(critical_packets);
      result->pyrowave_record_starts = std::move(record_starts);
      return result;
    }

    /**
     * @brief Identify the wire capability required by a validated PyroWave configuration.
     * @param config Validated stream configuration.
     * @return ServerCodecModeSupport bit for this profile.
     */
    uint32_t profile_flag(const config_t &config) {
      return config.chromaSamplingType ? (config.dynamicRange ? scm_hdr_444 : scm_sdr_444) :
                                         (config.dynamicRange ? scm_hdr : scm_sdr);
    }

    bool supported(const config_t &config, uint32_t profiles) {
      return valid_config(config) && (profiles & profile_flag(config)) != 0;
    }

    uint32_t probe_profiles(const std::function<bool(const config_t &)> &validate) {
      config_t probe {1920, 1080, 60, 6000, 200000, 1, 0, 3, video_format, 0, 0, 0};
      uint32_t profiles = 0;
      for (int chroma : {0, 1}) {
        for (int hdr : {0, 1}) {
          probe.chromaSamplingType = chroma;
          probe.dynamicRange = hdr;
          if (validate(probe)) {
            profiles |= profile_flag(probe);
            BOOST_LOG(info) << "Found PyroWave encoder (" << (hdr ? "HDR" : "SDR") << ", " << (chroma ? "4:4:4" : "4:2:0") << ')';
          }
        }
      }
      return profiles;
    }

    AVPixelFormat capture_format(const encoder_platform_formats_t &formats, const config_t &config) {
      // The shared D3D11 converter supports packed 4:4:4 independently of the hardware video encoder.
      if (formats.dev_type == platf::mem_type_e::dxgi) {
        return config.chromaSamplingType ? (config.dynamicRange ? AV_PIX_FMT_XV30 : AV_PIX_FMT_VUYX) :
                                           (config.dynamicRange ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12);
      }
      const auto avcodec_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(&formats);
      if (!avcodec_formats) {
        return AV_PIX_FMT_NONE;
      }
      return config.chromaSamplingType ? (config.dynamicRange ? avcodec_formats->avcodec_pix_fmt_yuv444_10bit : avcodec_formats->avcodec_pix_fmt_yuv444_8bit) :
                                         (config.dynamicRange ? avcodec_formats->avcodec_pix_fmt_10bit : avcodec_formats->avcodec_pix_fmt_8bit);
    }

    static_assert(PYROWAVE_API_VERSION_MAJOR == 1, "Sunshine requires PyroWave C API 1.x");

    bool map_gpu_frame(AVPixelFormat format, std::span<const pyrowave_image_view> images, pyrowave_gpu_buffers &buffers, float &scaling_factor) {
      buffers = {};
      scaling_factor = 1.0f;
      if (images.empty() || std::any_of(images.begin(), images.end(), [](const auto &image) {
            return !image.image || !image.width || !image.height;
          })) {
        return false;
      }

      if (format == AV_PIX_FMT_VUYX || format == AV_PIX_FMT_XV30) {
        std::array<VkComponentSwizzle, 3> swizzles;
        if (images.size() != 1) {
          return false;
        }
        if (format == AV_PIX_FMT_VUYX && images[0].image_format == VK_FORMAT_R8G8B8A8_UNORM) {
          swizzles = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R};
        } else if (format == AV_PIX_FMT_XV30 && images[0].image_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
          swizzles = {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_B};
        } else if (format == AV_PIX_FMT_XV30 && images[0].image_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
          swizzles = {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_R};
        } else {
          return false;
        }
        for (int component = 0; component < 3; ++component) {
          buffers.planes[component] = images[0];
          buffers.planes[component].view_format = images[0].image_format;
          buffers.planes[component].swizzle = swizzles[component];
        }
        return true;
      }

      VkFormat combined;
      std::array<VkFormat, 3> plane_formats {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM};
      const bool semiplanar = format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010;
      switch (format) {
        case AV_PIX_FMT_NV12:
          combined = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
          plane_formats[1] = VK_FORMAT_R8G8_UNORM;
          break;
        case AV_PIX_FMT_P010:
          combined = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
          plane_formats = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_UNDEFINED};
          scaling_factor = 65535.0f / (1023.0f * 64.0f);
          break;
        case AV_PIX_FMT_YUV420P:
          combined = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
          break;
        case AV_PIX_FMT_YUV444P:
          combined = VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
          break;
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_YUV444P16:
          combined = format == AV_PIX_FMT_YUV420P10 ? VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM : VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM;
          plane_formats.fill(VK_FORMAT_R16_UNORM);
          scaling_factor = format == AV_PIX_FMT_YUV444P16 ? 1.0f : 65535.0f / 1023.0f;
          break;
        default:
          return false;
      }

      const bool multiplanar = images.size() == 1;
      if (multiplanar ? images[0].image_format != combined : images.size() != (semiplanar ? 2 : 3)) {
        return false;
      }
      // Native P010 plane views normalize the ten significant bits without a shader scale.
      if (multiplanar && format == AV_PIX_FMT_P010) {
        plane_formats = {VK_FORMAT_R10X6_UNORM_PACK16, VK_FORMAT_R10X6G10X6_UNORM_2PACK16, VK_FORMAT_UNDEFINED};
        scaling_factor = 1.0f;
      }
      const auto desc = av_pix_fmt_desc_get(format);
      constexpr VkImageAspectFlagBits aspects[] {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_ASPECT_PLANE_2_BIT};
      for (int component = 0; component < 3; ++component) {
        const int plane = semiplanar && component ? 1 : component;
        const auto &image = images[multiplanar ? 0 : plane];
        if (!multiplanar && (image.image_format != plane_formats[plane] || image.width != (images[0].width >> (plane ? desc->log2_chroma_w : 0)) || image.height != (images[0].height >> (plane ? desc->log2_chroma_h : 0)))) {
          return false;
        }
        auto &view = buffers.planes[component];
        view = image;
        view.view_format = plane_formats[plane];
        view.aspect = multiplanar ? aspects[plane] : VK_IMAGE_ASPECT_COLOR_BIT;
        view.swizzle = semiplanar && component == 2 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_R;
      }
      return true;
    }

#ifdef SUNSHINE_BUILD_VULKAN
    /**
     * @brief Encode FFmpeg Vulkan frames using their device, layouts, and timeline semaphores.
     */
    class vulkan_input_t final: public gpu_input_t {
    public:
      using gpu_input_t::gpu_input_t;

      /**
       * @brief Finish submitted work before releasing command resources and the borrowed device wrapper.
       */
      ~vulkan_input_t() override {
        if (pending) {
          vkWaitForFences(vk->act_dev, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        if (command_pool) {
          vkDestroyCommandPool(vk->act_dev, command_pool, nullptr);
        }
        if (fence) {
          vkDestroyFence(vk->act_dev, fence, nullptr);
        }
        if (device) {
          api.destroy_device(device);
        }
      }

      /**
       * @brief Use FFmpeg to retain or derive a Vulkan device for hardware-only frame transfers.
       * @param source_frames Capture conversion pool.
       * @return True when the shared device and submission resources are usable.
       */
      bool init(AVHWFramesContext *source_frames) {
        if (!api.share_device || !source_frames->device_ctx) {
          return false;
        }
        const auto type = source_frames->device_ctx->type;
        external_frames = type == AV_HWDEVICE_TYPE_VAAPI;
        if (type != AV_HWDEVICE_TYPE_VULKAN && type != AV_HWDEVICE_TYPE_CUDA && type != AV_HWDEVICE_TYPE_VAAPI) {
          return false;
        }
        if (av_hwdevice_ctx_create_derived(&context, AV_HWDEVICE_TYPE_VULKAN, source_frames->device_ref, 0) < 0) {
          return false;
        }
        if (type != AV_HWDEVICE_TYPE_VULKAN) {
          transfer_frames.reset(av_hwframe_ctx_alloc(context.get()));
          if (!transfer_frames) {
            return false;
          }
          auto pool = reinterpret_cast<AVHWFramesContext *>(transfer_frames->data);
          pool->format = AV_PIX_FMT_VULKAN;
          pool->sw_format = source_frames->sw_format;
          pool->width = source_frames->width;
          pool->height = source_frames->height;
          auto vk_frames = static_cast<AVVulkanFramesContext *>(pool->hwctx);
          // CUDA interop requires separate images. VAAPI exports separate DMA-BUF layers.
          vk_frames->flags = AVVkFrameFlags(AV_VK_FRAME_FLAG_NONE | AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE);
          if (av_hwframe_ctx_init(transfer_frames.get()) < 0) {
            return false;
          }
          mapped.reset(av_frame_alloc());
          if (!mapped) {
            return false;
          }
        }
        auto ctx = reinterpret_cast<AVHWDeviceContext *>(context->data);
        vk = static_cast<AVVulkanDeviceContext *>(ctx->hwctx);
        if (!vk->get_proc_addr || vk->alloc) {
          return false;
        }
  #if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100)
        synchronized_queues = vk->queue_flags & VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
  #endif
  #if !FF_API_VULKAN_SYNC_QUEUES
        if (!synchronized_queues) {
          return false;
        }
  #endif
        for (auto capability : {VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT}) {
          const auto family = std::find_if(vk->qf, vk->qf + vk->nb_qf, [&](const auto &qf) {
            return qf.num > 0 && (qf.flags & capability);
          });
          if (family == vk->qf + vk->nb_qf) {
            return false;
          }
          if (capability == VK_QUEUE_COMPUTE_BIT) {
            compute_family = family->idx;
          }
          if (queue_count && queues[0].familyIndex == uint32_t(family->idx)) {
            continue;
          }
          auto &queue = queues[queue_count];
          auto &info = queue_infos[queue_count++];
          info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
          info.queueFamilyIndex = family->idx;
          info.queueCount = 1;
          info.pQueuePriorities = &priority;
  #if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100)
          info.flags = vk->queue_flags;
  #endif
          VkDeviceQueueInfo2 get_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
          get_info.flags = info.flags;
          get_info.queueFamilyIndex = family->idx;
          queue.familyIndex = family->idx;
          vkGetDeviceQueue2(vk->act_dev, &get_info, &queue.queue);
          if (!queue.queue) {
            return false;
          }
        }
        for (uint32_t i = 0; i < queue_count; ++i) {
          if (queues[i].familyIndex == compute_family) {
            compute_queue = queues[i].queue;
          }
        }
        application.apiVersion = VK_API_VERSION_1_3;
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = vk->nb_enabled_inst_extensions;
        instance_info.ppEnabledExtensionNames = vk->enabled_inst_extensions;
        device_info.pNext = &vk->device_features;
        device_info.enabledExtensionCount = vk->nb_enabled_dev_extensions;
        device_info.ppEnabledExtensionNames = vk->enabled_dev_extensions;
        device_info.queueCreateInfoCount = queue_count;
        device_info.pQueueCreateInfos = queue_infos.data();
        pyrowave_device_create_info info {};
        info.GetInstanceProcAddr = vk->get_proc_addr;
        info.instance = vk->inst;
        info.physical_device = vk->phys_dev;
        info.device = vk->act_dev;
        info.instance_create_info = &instance_info;
        info.device_create_info = &device_info;
        info.queue_info = queues.data();
        info.queue_info_count = queue_count;
        info.userdata = this;
        info.queue_lock_callback = [](void *self) {
          static_cast<vulkan_input_t *>(self)->lock_queues(true);
        };
        info.queue_unlock_callback = [](void *self) {
          static_cast<vulkan_input_t *>(self)->lock_queues(false);
        };
        if (api.share_device(&info, &device) != PYROWAVE_SUCCESS || api.set_queue_type(device, VK_QUEUE_COMPUTE_BIT) != PYROWAVE_SUCCESS) {
          return false;
        }
        VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = compute_family;
        VkCommandBufferAllocateInfo allocation {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateCommandPool(vk->act_dev, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
          return false;
        }
        allocation.commandPool = command_pool;
        return vkAllocateCommandBuffers(vk->act_dev, &allocation, &command_buffer) == VK_SUCCESS &&
               vkCreateFence(vk->act_dev, &fence_info, nullptr, &fence) == VK_SUCCESS;
      }

      /** @copydoc gpu_input_t::prepare */
      bool prepare(AVFrame *source) override {
        if (!source->hw_frames_ctx) {
          return false;
        }
        if (source->format == AV_PIX_FMT_CUDA && transfer_frames) {
          if ((!mapped->buf[0] && av_hwframe_get_buffer(transfer_frames.get(), mapped.get(), 0) < 0) || av_hwframe_transfer_data(mapped.get(), source, 0) < 0) {
            return false;
          }
          source = mapped.get();
        } else if (source->format == AV_PIX_FMT_VAAPI && transfer_frames) {
          av_frame_unref(mapped.get());
          mapped->format = AV_PIX_FMT_VULKAN;
          mapped->hw_frames_ctx = av_buffer_ref(transfer_frames.get());
          if (!mapped->hw_frames_ctx || av_hwframe_map(mapped.get(), source, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0) {
            return false;
          }
          source = mapped.get();
        }
        if (source->format != AV_PIX_FMT_VULKAN || !source->data[0]) {
          return false;
        }
        frame = source;
        auto pool = reinterpret_cast<AVHWFramesContext *>(source->hw_frames_ctx->data);
        if (pool->device_ctx->hwctx != vk) {
          return false;
        }
        auto vk_frames = static_cast<AVVulkanFramesContext *>(pool->hwctx);
        auto pixels = reinterpret_cast<AVVkFrame *>(source->data[0]);
        const auto desc = av_pix_fmt_desc_get(pool->sw_format);
        if (!desc || !(vk_frames->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
          return false;
        }
        std::array<pyrowave_image_view, AV_NUM_DATA_POINTERS> images {};
        size_t count = 0;
        // FFmpeg uses concurrent sharing when more than one distinct queue family is enabled.
        const bool concurrent = std::any_of(vk->qf, vk->qf + vk->nb_qf, [&](const auto &qf) {
          return qf.idx != int(compute_family);
        });
        image_family = concurrent ? VK_QUEUE_FAMILY_IGNORED : compute_family;
        for (; count < images.size() && pixels->img[count]; ++count) {
          if (pixels->queue_family[count] == VK_QUEUE_FAMILY_EXTERNAL || pixels->queue_family[count] == VK_QUEUE_FAMILY_FOREIGN_EXT || (!concurrent && pixels->queue_family[count] != compute_family && pixels->queue_family[count] != VK_QUEUE_FAMILY_IGNORED)) {
            return false;
          }
          auto &image = images[count];
          image.image = pixels->img[count];
          image.image_format = vk_frames->format[count];
          image.width = source->width >> (count ? desc->log2_chroma_w : 0);
          image.height = source->height >> (count ? desc->log2_chroma_h : 0);
          image.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
          image.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        return map_gpu_frame(pool->sw_format, std::span(images).first(count), buffers, scaling_factor);
      }

      /** @copydoc gpu_input_t::encode */
      pyrowave_result encode(pyrowave_encoder encoder, const pyrowave_rate_control &rate_control) override {
        auto pool = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
        auto vk_frames = static_cast<AVVulkanFramesContext *>(pool->hwctx);
        auto pixels = reinterpret_cast<AVVkFrame *>(frame->data[0]);
        vk_frames->lock_frame(pool, pixels);
        auto unlock = util::fail_guard([&]() {
          vk_frames->unlock_frame(pool, pixels);
        });
        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandPool(vk->act_dev, command_pool, 0) != VK_SUCCESS || vkResetFences(vk->act_dev, 1, &fence) != VK_SUCCESS || vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
          return PYROWAVE_ERROR_GENERIC;
        }
        std::array<VkImageMemoryBarrier2, AV_NUM_DATA_POINTERS> barriers {};
        std::array<VkSemaphoreSubmitInfo, AV_NUM_DATA_POINTERS> waits {}, signals {};
        uint32_t count = 0;
        for (; count < barriers.size() && pixels->img[count]; ++count) {
          auto &barrier = barriers[count];
          barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
          barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
          barrier.srcAccessMask = pixels->access[count];
          barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
          barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
          barrier.oldLayout = pixels->layout[count];
          barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          if (external_frames && pixels->queue_family[count] == VK_QUEUE_FAMILY_EXTERNAL) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = 0;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            barrier.dstQueueFamilyIndex = image_family;
          }
          barrier.image = pixels->img[count];
          barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          if (!pixels->sem[count]) {
            return PYROWAVE_ERROR_INVALID_ARGUMENT;
          }
          waits[count] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
          waits[count].semaphore = pixels->sem[count];
          waits[count].value = pixels->sem_value[count];
          waits[count].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
          signals[count] = waits[count];
          ++signals[count].value;
        }
        VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = count;
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        api.set_scaling_factor(encoder, scaling_factor);
        api.set_command_buffer(device, command_buffer);
        const auto result = api.encode_gpu(encoder, nullptr, nullptr, &buffers, &rate_control);
        api.set_command_buffer(device, VK_NULL_HANDLE);
        if (result != PYROWAVE_SUCCESS) {
          return result;
        }
        if (external_frames) {
          // VAAPI/EGL may overwrite the surface on the next capture conversion.
          for (uint32_t i = 0; i < count; ++i) {
            auto &barrier = barriers[i];
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = image_family;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
          }
          vkCmdPipelineBarrier2(command_buffer, &dependency);
        }
        if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
          return PYROWAVE_ERROR_GENERIC;
        }
        VkCommandBufferSubmitInfo command {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = command_buffer;
        VkSubmitInfo2 submission {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submission.waitSemaphoreInfoCount = count;
        submission.pWaitSemaphoreInfos = waits.data();
        submission.signalSemaphoreInfoCount = count;
        submission.pSignalSemaphoreInfos = signals.data();
        submission.commandBufferInfoCount = 1;
        submission.pCommandBufferInfos = &command;
        lock_queues(true);
        const auto submitted = vkQueueSubmit2(compute_queue, 1, &submission, fence);
        lock_queues(false);
        if (submitted != VK_SUCCESS) {
          return PYROWAVE_ERROR_GENERIC;
        }
        pending = true;
        for (uint32_t i = 0; i < count; ++i) {
          pixels->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
          pixels->access[i] = external_frames ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
          pixels->queue_family[i] = external_frames ? VK_QUEUE_FAMILY_EXTERNAL : image_family;
          pixels->sem_value[i] = signals[i].value;
        }
        vk_frames->unlock_frame(pool, pixels);
        unlock.disable();
        // Packetization reads encoded bytes on the CPU, so this submission must finish first.
        if (vkWaitForFences(vk->act_dev, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
          return PYROWAVE_ERROR_GENERIC;
        }
        pending = false;
        return PYROWAVE_SUCCESS;
      }

    private:
      /**
       * @brief Synchronize queue submissions with FFmpeg on devices without internally synchronized queues.
       * @param lock Whether to acquire or release the queue locks.
       */
      void lock_queues(bool lock) {
  #if FF_API_VULKAN_SYNC_QUEUES
        if (synchronized_queues) {
          return;
        }
          // Older FFmpeg devices still require the queue locks deprecated by the new queue extension.
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        auto ctx = reinterpret_cast<AVHWDeviceContext *>(context->data);
        for (uint32_t i = 0; i < queue_count; ++i) {
          const auto &queue = queues[lock ? i : queue_count - i - 1];
          if (lock) {
            vk->lock_queue(ctx, queue.familyIndex, queue.index);
          } else {
            vk->unlock_queue(ctx, queue.familyIndex, queue.index);
          }
        }
    #pragma GCC diagnostic pop
  #endif
      }

      avcodec_buffer_t context;  ///< Retained FFmpeg Vulkan device and feature chain.
      avcodec_buffer_t transfer_frames;  ///< Optional pool for hardware-to-hardware transfers.
      avcodec_frame_t mapped;  ///< Imported VAAPI frame or reusable CUDA transfer target.
      AVVulkanDeviceContext *vk = nullptr;  ///< Borrowed context owned by context.
      AVFrame *frame = nullptr;  ///< Prepared frame owned by the converter or mapped.
      VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};  ///< Borrowed instance API version.
      VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};  ///< Enabled instance extensions.
      VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};  ///< Enabled device features and queues.
      std::array<VkDeviceQueueCreateInfo, 2> queue_infos {};  ///< Graphics and compute queue descriptions.
      std::array<pyrowave_device_create_queue_info, 2> queues {};  ///< Concrete borrowed queue handles.
      uint32_t queue_count = 0;  ///< Number of distinct borrowed queue families.
      float priority = 1.0f;  ///< Queue priority storage retained with the create infos.
      uint32_t compute_family = 0;  ///< Queue family used for the encoding command buffer.
      VkQueue compute_queue {};  ///< Borrowed compute queue.
      VkCommandPool command_pool {};  ///< Reusable command buffer allocation pool.
      VkCommandBuffer command_buffer {};  ///< Current encoding command buffer.
      VkFence fence {};  ///< Completion of the current encoding submission.
      bool pending = false;  ///< Whether the command pool still has submitted work.
      bool synchronized_queues = false;  ///< Whether the driver synchronizes shared queue submissions.
      bool external_frames = false;  ///< Whether imported VAAPI images must be released back to their producer.
      uint32_t image_family = VK_QUEUE_FAMILY_IGNORED;  ///< Ownership while Vulkan samples the images.
      pyrowave_gpu_buffers buffers {};  ///< Sampled YUV component views.
      float scaling_factor = 1.0f;  ///< Normalization of the GPU sample storage.
    };
#endif

    /**
     * @brief Own conversion, GPU codec state, and reusable host transfer buffers.
     */
    class session_impl_t final: public session_t {
    public:
      /**
       * @brief Store the converter and byte budget before initializing the codec.
       *
       * @param device Initialized frame conversion device.
       * @param budget Maximum native bitstream bytes per frame, including record headers.
       * @param api C API implementation to call.
       * @param frames Hardware frame pool retained until after converter destruction.
       */
      session_impl_t(std::unique_ptr<platf::avcodec_encode_device_t> device, size_t budget, const api_t &api, avcodec_buffer_t frames):
          frames {std::move(frames)},
          converter {std::move(device)},
          api {api},
          rate_control {budget} {
      }

      /**
       * @brief Destroy codec objects in dependency order, including partial initialization.
       */
      ~session_impl_t() override {
        if (encoder) {
          api.destroy_encoder(encoder);
        }
        if (device && !gpu_input) {
          api.destroy_device(device);
        }
      }

      /**
       * @brief Initialize GPU sharing when available and prepare the CPU fallback for the negotiated profile.
       *
       * @param config Negotiated stream configuration.
       * @return True when the Vulkan device and encoder are usable.
       */
      bool init(const config_t &config) {
        framing = config;
        uint32_t major, minor, patch;
        api.get_api_version(&major, &minor, &patch);
        if (major != PYROWAVE_API_VERSION_MAJOR || std::pair {major, minor} < std::pair {uint32_t(PYROWAVE_API_VERSION_MAJOR), uint32_t(PYROWAVE_API_VERSION_MINOR)}) {
          BOOST_LOG(error) << "Incompatible PyroWave C API: " << major << '.' << minor << '.' << patch;
          return false;
        }
#if defined(SUNSHINE_BUILD_VULKAN) || defined(_WIN32)
        const auto pool_ref = converter->frame->hw_frames_ctx ? converter->frame->hw_frames_ctx : frames.get();
        if (pool_ref) {
          auto pool = reinterpret_cast<AVHWFramesContext *>(pool_ref->data);
  #ifdef SUNSHINE_BUILD_VULKAN
          auto input = std::make_unique<vulkan_input_t>(api);
          if (input->init(pool)) {
            gpu_input = std::move(input);
          }
  #endif
  #ifdef _WIN32
          if (pool->format == AV_PIX_FMT_D3D11 && api.match_device) {
            gpu_input = make_d3d11_input(pool, api);
          }
  #endif
        }
#endif
        const auto create_encoder = [&]() {
          const pyrowave_encoder_create_info info {device, config.width, config.height, config.chromaSamplingType ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420};
          return api.create_encoder(&info, &encoder) == PYROWAVE_SUCCESS;
        };
        if (gpu_input) {
          device = gpu_input->device;
          if (!create_encoder()) {
            if (encoder) {
              api.destroy_encoder(encoder);
              encoder = nullptr;
            }
            gpu_input.reset();
            device = nullptr;
          }
        }
        if (!gpu_input) {
          if (api.create_device(&device) != PYROWAVE_SUCCESS) {
            BOOST_LOG(error) << "Could not create a PyroWave Vulkan device";
            return false;
          }
          if (!create_encoder()) {
            BOOST_LOG(error) << "Could not create a PyroWave encoder";
            return false;
          }
        }
        gpu_active = gpu_input != nullptr;

        converted.reset(av_frame_alloc());
        readback.reset(av_frame_alloc());
        input_frame.reset(av_frame_alloc());
        if (!converted || !readback || !input_frame) {
          return false;
        }
        auto source_format = converter->frame->format;
        if (const auto pool = converter->frame->hw_frames_ctx ? converter->frame->hw_frames_ctx : frames.get()) {
          source_format = reinterpret_cast<AVHWFramesContext *>(pool->data)->sw_format;
        }
        // PyroWave caches upload images, so keep the NV12 or planar layout fixed for this session.
        converted->format = config.chromaSamplingType ? (config.dynamicRange ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P) :
                                                        (config.dynamicRange ? AV_PIX_FMT_YUV420P10 : (source_format == AV_PIX_FMT_NV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P));
        converted->width = config.width;
        converted->height = config.height;
        return av_frame_copy_props(converted.get(), converter->frame) >= 0;
      }

      /**
       * @brief Retain compatible frames directly, converting other layouts while preserving HDR precision.
       *
       * @param img Captured image.
       * @return Zero on success, or a negative conversion/transfer error.
       */
      int convert(platf::img_t &img) override {
        ready = false;
        av_frame_unref(input_frame.get());
        if (converter->convert(img)) {
          return -1;
        }
        auto source = converter->frame;
        if (gpu_active) {
          const auto pool = source->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext *>(source->hw_frames_ctx->data) : nullptr;
          if (pool && source->width == converted->width && source->height == converted->height && matches_profile(pool->sw_format) && av_frame_ref(input_frame.get(), source) >= 0 && gpu_input->prepare(input_frame.get())) {
            ready = true;
            return 0;
          }
          av_frame_unref(input_frame.get());
          gpu_active = false;
          BOOST_LOG(warning) << "PyroWave GPU input unavailable; using CPU frame transfer";
        }
        if (source->hw_frames_ctx) {
          av_frame_unref(readback.get());
          if (av_hwframe_transfer_data(readback.get(), source, 0) < 0) {
            BOOST_LOG(error) << "Could not download the PyroWave input frame";
            return -1;
          }
          source = readback.get();
        }

        // Only change plane layout/bit storage here. The capture converter has
        // already applied the negotiated matrix, range, and HDR transfer function.
        if (source->format != AV_PIX_FMT_NV12 && source->format != AV_PIX_FMT_P010 && source->format != AV_PIX_FMT_YUV420P && source->format != AV_PIX_FMT_YUV420P10 && source->format != AV_PIX_FMT_YUV444P && source->format != AV_PIX_FMT_YUV444P10 && source->format != AV_PIX_FMT_YUV444P16 && source->format != AV_PIX_FMT_VUYX && source->format != AV_PIX_FMT_XV30) {
          BOOST_LOG(error) << "Unsupported PyroWave input pixel format: " << source->format;
          return -1;
        }
        if (!matches_profile(AVPixelFormat(source->format))) {
          BOOST_LOG(error) << "PyroWave input color depth or chroma sampling does not match the negotiated profile";
          return -1;
        }
        // Matching layouts and dimensions can bypass sws_scale().
        // PyroWave requires positive row strides, so reversed rows still need conversion.
        const bool direct = source->format == converted->format && source->width == converted->width && source->height == converted->height &&
                            std::all_of(source->linesize, source->linesize + av_pix_fmt_count_planes(AVPixelFormat(source->format)), [](int stride) {
                              return stride > 0;
                            });
        if (!direct) {
          auto context = sws_getCachedContext(scaler.release(), source->width, source->height, AVPixelFormat(source->format), converted->width, converted->height, AVPixelFormat(converted->format), SWS_POINT, nullptr, nullptr, nullptr);
          scaler.reset(context);
          if (!scaler || (!converted->buf[0] && av_frame_get_buffer(converted.get(), 0) < 0) || av_frame_make_writable(converted.get()) < 0 || sws_scale(scaler.get(), source->data, source->linesize, 0, source->height, converted->data, converted->linesize) != converted->height) {
            return -1;
          }
          source = converted.get();
        }
        // References keep repeated frames alive; av_frame_ref() copies unowned capture buffers.
        if (av_frame_ref(input_frame.get(), source) < 0) {
          return -1;
        }
        ready = true;
        return 0;
      }

      /**
       * @brief Every PyroWave frame is independently decodable.
       */
      void request_idr_frame() override {}

      /**
       * @brief Continue producing independently decodable frames.
       */
      void request_normal_frame() override {}

      /**
       * @brief Ignore reference invalidation because the codec has no inter-frame references.
       *
       * @param first_frame First lost frame, unused.
       * @param last_frame Last lost frame, unused.
       */
      void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {}

      /**
       * @brief Encode the latest converted pixels and preserve native packet boundaries.
       *
       * @param frame_index GameStream frame sequence number.
       * @return Independent frame container, or null on codec failure.
       */
      packet_t encode(int64_t frame_index) override {
        if (!ready) {
          return nullptr;
        }
        pyrowave_result result;
        if (gpu_active) {
          result = gpu_input->encode(encoder, rate_control);
        } else {
          pyrowave_cpu_buffer input {};
          input.width = input_frame->width;
          input.height = input_frame->height;
          const auto desc = av_pix_fmt_desc_get(AVPixelFormat(input_frame->format));
          input.format = input_frame->format == AV_PIX_FMT_NV12 ? PYROWAVE_CPU_BUFFER_FORMAT_NV12 :
                         desc->log2_chroma_w == 0               ? (desc->comp[0].depth == 10 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV444P) :
                                                                  (desc->comp[0].depth == 10 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P);
          for (int plane = 0; plane < av_pix_fmt_count_planes(AVPixelFormat(input_frame->format)); ++plane) {
            input.data[plane] = input_frame->data[plane];
            input.row_stride_in_bytes[plane] = input_frame->linesize[plane];
            input.plane_size_in_bytes[plane] = input.row_stride_in_bytes[plane] * (input.height >> (plane ? desc->log2_chroma_h : 0));
          }
          result = api.encode_cpu(encoder, &input, &rate_control);
        }
        size_t count = 0;
        const auto boundary = framing.pyrowaveRecordFraming ? record_packet_boundary : packet_boundary;
        if (result != PYROWAVE_SUCCESS || api.num_packets(encoder, boundary, &count) != PYROWAVE_SUCCESS || count == 0 || count > max_frame_size / boundary) {
          BOOST_LOG(error) << "PyroWave encoding failed";
          return nullptr;
        }

        packets.resize(count);
        // Native records cannot be split, so a legacy packet can exceed its 1024-byte target.
        // Query an allocation with a boundary large enough for any native record.
        size_t storage_count = count;
        if (boundary != record_packet_boundary && (api.num_packets(encoder, record_packet_boundary, &storage_count) != PYROWAVE_SUCCESS || !storage_count || storage_count > max_frame_size / record_packet_boundary)) {
          return nullptr;
        }
        bitstream.resize(storage_count * record_packet_boundary);
        if (api.packetize(encoder, packets.data(), boundary, &count, bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS || count == 0 || count > packets.size()) {
          BOOST_LOG(error) << "PyroWave packetization failed";
          return nullptr;
        }

        size_t critical_blocks = 0;
        // The first two native bands contain the coarsest wavelet level and its detail.
        if (framing.pyrowaveRecordFraming && api.active_blocks(encoder, 2, &critical_blocks) != PYROWAVE_SUCCESS) {
          return nullptr;
        }
        return frame_packets(std::span(packets.data(), count), bitstream, framing, critical_blocks, frame_index);
      }

    private:
      /**
       * @brief Match eight-bit versus higher precision and chroma sampling to the session.
       *
       * @param format Pixel layout supplied by the converter or hardware frame pool.
       * @return True when the depth category and chroma subsampling match.
       */
      bool matches_profile(AVPixelFormat format) const {
        const auto input = av_pix_fmt_desc_get(format);
        const auto output = av_pix_fmt_desc_get(AVPixelFormat(converted->format));
        return input && (input->comp[0].depth > 8) == (output->comp[0].depth > 8) && input->log2_chroma_w == output->log2_chroma_w && input->log2_chroma_h == output->log2_chroma_h;
      }

      avcodec_buffer_t frames;  ///< Hardware pool borrowed by the converter; destroyed last.
      std::unique_ptr<platf::avcodec_encode_device_t> converter;  ///< Platform color conversion and source frame owner.
      api_t api;  ///< Runtime C API implementation.
      std::unique_ptr<gpu_input_t> gpu_input;  ///< Optional hardware sharing resources, retained through encoder destruction.
      bool gpu_active = false;  ///< Whether subsequent frames can use hardware sharing.
      pyrowave_device device {};  ///< Device owned by gpu_input, or owned directly for CPU input.
      pyrowave_encoder encoder {};  ///< Codec instance owned by this session.
      pyrowave_rate_control rate_control;  ///< Per-frame coefficient byte budget.
      config_t framing {};  ///< Client framing and final negotiated packet size.
      avcodec_frame_t readback;  ///< Downloaded hardware frame, when needed.
      avcodec_frame_t converted;  ///< Stable CPU input layout, with buffers allocated only when conversion is needed.
      avcodec_frame_t input_frame;  ///< Retained pixels for encoding, including repeated frames.
      sws_t scaler;  ///< Plane-layout and bit-storage conversion preserving negotiated chroma and color depth.
      std::vector<pyrowave_packet> packets;  ///< Packet offsets returned by the C API.
      std::vector<uint8_t> bitstream;  ///< Reusable encoded coefficient storage.
      bool ready = false;  ///< Whether conversion has produced a usable frame.
    };

    std::unique_ptr<session_t> make_session(const config_t &config, std::unique_ptr<platf::avcodec_encode_device_t> device, const api_t &api, avcodec_buffer_t frames) {
      if (!valid_config(config) || !device || !device->frame || device->frame->width != config.width || device->frame->height != config.height) {
        return nullptr;
      }
      // Associate borrowed VideoToolbox buffers with their pool for readback.
      if (device->frame->format == AV_PIX_FMT_VIDEOTOOLBOX && !device->frame->hw_frames_ctx) {
        if (!frames) {
          return nullptr;
        }
        device->frame->hw_frames_ctx = av_buffer_ref(frames.get());
        if (!device->frame->hw_frames_ctx) {
          return nullptr;
        }
      }
      auto session = std::make_unique<session_impl_t>(std::move(device), frame_budget(config), api, std::move(frames));
      return session->init(config) ? std::move(session) : nullptr;
    }

    std::unique_ptr<session_t> make_session(const config_t &config, std::unique_ptr<platf::avcodec_encode_device_t> device, avcodec_buffer_t frames) {
      return make_session(config, std::move(device), api_t {}, std::move(frames));
    }
  }  // namespace pyrowave

  /**
   * @brief Native VideoToolbox encode session and device state for hardware encoding.
   */
  class vt_encode_session_t: public encode_session_t {
  public:
    /**
     * @brief Initialize a VideoToolbox encode session and take ownership of its device.
     *
     * @param encode_device Encode device.
     */
    vt_encode_session_t(std::unique_ptr<platf::vt_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    /**
     * @brief Supply the next captured image to the encode device.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      force_idr = true;
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      force_idr = false;
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     * @note VideoToolbox has no reference frame invalidation; recover with an IDR frame instead.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      force_idr = true;
    }

    /**
     * @brief Submit the next frame to VideoToolbox and return the encoded payload.
     *
     * @param frame_index Monotonic frame index assigned by the video pipeline.
     * @return Encoded frame payload and frame metadata.
     */
    platf::vt_encoded_frame encode_frame(uint64_t frame_index) {
      if (!device) {
        return {};
      }

      auto result = device->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::vt_encode_device_t> device;
    bool force_idr = false;
  };

  /**
   * @brief Context object used while synchronizing encode sessions.
   */
  struct sync_session_ctx_t {
    safe::signal_t *join_event;  ///< Signal raised when the capture and encode workers should join.
    safe::mail_raw_t::event_t<bool> shutdown_event;  ///< Event raised when the stream should shut down.
    safe::mail_raw_t::queue_t<packet_t> packets;  ///< Queue receiving encoded video packets for the stream sender.
    safe::mail_raw_t::event_t<bool> idr_events;  ///< Event raised when an IDR frame is requested.
    safe::mail_raw_t::event_t<hdr_info_t> hdr_events;  ///< Event carrying updated HDR metadata.
    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events;  ///< Event carrying updated touch viewport metadata.

    config_t config;  ///< Stream or encoder configuration captured for the worker.
    int frame_nr;  ///< Next capture-frame number assigned to encoded packets.
    stream::session_t *channel_data;  ///< Stream session forwarded to packet senders.
  };

  /**
   * @brief Synchronization state for one encode session.
   */
  struct sync_session_t {
    sync_session_ctx_t *ctx;  ///< Shared capture/encode synchronization context.
    std::unique_ptr<encode_session_t> session;  ///< Active encoder session used by the capture thread.
  };

  /**
   * @brief Queue of encode-session contexts waiting for capture work.
   */
  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;
  /**
   * @brief Platform capture status returned by encode operations.
   */
  using encode_e = platf::capture_e;

  /**
   * @brief Capture thread context shared with the encoder session.
   */
  struct capture_ctx_t {
    img_event_t images;  ///< Queue of captured images waiting for encode.
    config_t config;  ///< Stream or encoder configuration captured for the worker.
  };

  /**
   * @brief Asynchronous capture thread state.
   */
  struct capture_thread_async_ctx_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;  ///< Capture ctx queue.
    std::jthread capture_thread;  ///< Capture thread.

    safe::signal_t reinit_event;  ///< Reinit event.
    const encoder_t *encoder_p;  ///< Encoder p.
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;  ///< Display wp.
  };

  /**
   * @brief Synchronous capture thread state.
   */
  struct capture_thread_sync_ctx_t {
    encode_session_ctx_queue_t encode_session_ctx_queue {30};  ///< Encode session ctx queue.
  };

  /**
   * @brief Start the synchronous multi-client capture thread.
   *
   * @param ctx Unused state required by the shared capture lifetime callback signature.
   * @return 0 when the capture thread is started.
   */
  int start_capture_sync(capture_thread_sync_ctx_t &ctx);
  /**
   * @brief Leave synchronous capture cleanup to the capture thread.
   *
   * @param ctx Unused state required by the shared capture lifetime callback signature.
   */
  void end_capture_sync(capture_thread_sync_ctx_t &ctx);
  /**
   * @brief Start the asynchronous capture thread.
   *
   * @param ctx Native context object used by the operation or callback.
   * @return 0 when the capture thread is started; nonzero on setup failure.
   */
  int start_capture_async(capture_thread_async_ctx_t &ctx);
  /**
   * @brief Stop capture async processing.
   *
   * @param ctx Native context object used by the operation or callback.
   */
  void end_capture_async(capture_thread_async_ctx_t &ctx);

  // Keep a reference counter to ensure the capture thread only runs when other threads have a reference to the capture thread
  auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(start_capture_async, end_capture_async);  ///< Capture thread async.
  auto capture_thread_sync = safe::make_shared<capture_thread_sync_ctx_t>(start_capture_sync, end_capture_sync);  ///< Capture thread sync.

#ifdef _WIN32
  /**
   * @brief NVENC.
   */
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv,
      platf::pix_fmt_e::yuv444p16
    ),
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
      {},  // capabilities
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
      {},  // capabilities
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
      {},  // capabilities
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN  // flags
  };
#elif !defined(__APPLE__)
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
  #ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
  #else
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_CUDA,
  #endif
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P16,
  #ifdef _WIN32
      dxgi_init_avcodec_hardware_input_buffer
  #else
      cuda_init_avcodec_hardware_input_buffer
  #endif
    ),
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"spatial-aq"s, &config::video.nv_legacy.spatial_aq},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"spatial-aq"s, &config::video.nv_legacy.spatial_aq},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(nv::profile_hevc_e::main)},
      },
      {
        // HDR-specific options
        {"profile"s, std::to_underlying(nv::profile_hevc_e::main_10)},
      },
      // libavcodec will automatically use the rext profile for
      // YUV444 SDR and HDR content, so it doesn't need to be specified here
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
      {},  // capabilities
    },
    {
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"coder"s, &config::video.nv_legacy.h264_coder},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"spatial-aq"s, &config::video.nv_legacy.spatial_aq},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(nv::profile_h264_e::high)},
      },
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
      {},  // capabilities
    },
    PARALLEL_ENCODING | YUV444_SUPPORT
  };
#endif

#ifdef _WIN32
  /**
   * @brief Quicksync.
   */
  encoder_t quicksync {
    "quicksync"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_QSV,
      AV_PIX_FMT_QSV,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX,
      AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::main)},
      },
      {
        // HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::main)},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::high)},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::high)},
      },
      {},  // Fallback options
      "av1_qsv"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"pic_timing_sei"s, 0},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::main)},
      },
      {
        // HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::main_10)},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::rext)},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::rext)},
      },
      {
        // Fallback options
        {"low_power"s, []() {
           return config::video.qsv.qsv_slow_hevc ? 0 : 1;
         }},
      },
      "hevc_qsv"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"cavlc"s, &config::video.qsv.qsv_cavlc},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"vcm"s, 1},
        {"pic_timing_sei"s, 0},
        {"max_dec_frame_buffering"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_h264_e::high)},
      },
      {},  // HDR-specific options
      {
        // YUV444 SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_h264_e::high_444p)},
      },
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Some old/low-end Intel GPUs don't support low power encoding
      },
      "h264_qsv"s,
      {},  // capabilities
    },
    PARALLEL_ENCODING | CBR_WITH_VBR | RELAXED_COMPLIANCE | NO_RC_BUF_LIMIT | YUV444_SUPPORT
  };

  /**
   * @brief Amdvce.
   */
  encoder_t amdvce {
    "amdvce"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, "lowest_latency"s},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_av1},
        {"rc"s, &config::video.amd.amd_rc_av1},
        {"usage"s, &config::video.amd.amd_usage_av1},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"gops_per_idr"s, 1},
        {"header_insertion_mode"s, "idr"s},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_hevc},
        {"rc"s, &config::video.amd.amd_rc_hevc},
        {"usage"s, &config::video.amd.amd_usage_hevc},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"max_au_size"s, &config::video.amd.amd_max_au_size},
        {"level"s, [](const config_t &cfg) {
           // For 4K and below, try to use level 5.1 or 5.2 if possible
           if (auto size = cfg.width * cfg.height; size <= 8912896) {
             if (size * cfg.framerate <= 534773760) {
               return "5.1"s;
             } else if (size * cfg.framerate <= 1069547520) {
               return "5.2"s;
             }
           }
           return "auto"s;
         }},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_amf"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"frame_skipping"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_h264},
        {"rc"s, &config::video.amd.amd_rc_h264},
        {"usage"s, &config::video.amd.amd_usage_h264},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"coder"s, &config::video.amd.amd_coder},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"max_au_size"s, &config::video.amd.amd_max_au_size},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"usage"s, 2 /* AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY */},  // Workaround for https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/410
      },
      "h264_amf"s,
      {},  // capabilities
    },
    PARALLEL_ENCODING
  };

  /**
   * @brief Mediafoundation.
   */
  encoder_t mediafoundation {
    "mediafoundation"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,  // SDR 4:2:0 8-bit (only format Qualcomm supports)
      AV_PIX_FMT_NONE,  // No HDR - Qualcomm MF only supports 8-bit
      AV_PIX_FMT_NONE,  // No YUV444 SDR
      AV_PIX_FMT_NONE,  // No YUV444 HDR
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options for AV1 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_mf"s,
      {},  // capabilities
    },
    {
      // Common options for HEVC - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_mf"s,
      {},  // capabilities
    },
    {
      // Common options for H.264 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_mf"s,
      {},  // capabilities
    },
    PARALLEL_ENCODING | FIXED_GOP_SIZE  // MF encoder doesn't support on-demand IDR frames
  };
#endif

  /**
   * @brief Software.
   */
  encoder_t software {
    "software"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_NONE,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV420P,
      AV_PIX_FMT_YUV420P10,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P10,
      nullptr
    ),
    {
      // libsvtav1 takes different presets than libx264/libx265.
      // We set an infinite GOP length, use a low delay prediction structure,
      // force I frames to be key frames, and set max bitrate to default to work
      // around a FFmpeg bug with CBR mode.
      {
        {"svtav1-params"s, "keyint=-1:pred-struct=1:force-key-frames=1:mbr=0"s},
        {"preset"s, &config::video.sw.svtav1_preset},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options

#ifdef ENABLE_BROKEN_AV1_ENCODER
           // Due to bugs preventing on-demand IDR frames from working and very poor
           // real-time encoding performance, we do not enable libsvtav1 by default.
           // It is only suitable for testing AV1 until the IDR frame issue is fixed.
      "libsvtav1"s,
#else
      {},
#endif
      {},  // capabilities
    },
    {
      // x265's Info SEI is so long that it causes the IDR picture data to be
      // kicked to the 2nd packet in the frame, breaking Moonlight's parsing logic.
      // It also looks like gop_size isn't passed on to x265, so we have to set
      // 'keyint=-1' in the parameters ourselves.
      {
        {"forced-idr"s, 1},
        {"x265-params"s, "info=0:keyint=-1"s},
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx265"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx264"s,
      {},  // capabilities
    },
    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT
  };

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  /**
   * @brief VA-API.
   */
  encoder_t vaapi {
    "vaapi"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VAAPI,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vaapi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"blbrc"s, &config::video.vaapi.blbrc},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vaapi"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"blbrc"s, &config::video.vaapi.blbrc},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vaapi"s,
      {},  // capabilities
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"blbrc"s, &config::video.vaapi.blbrc},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vaapi"s,
      {},  // capabilities
    },
    // RC buffer size will be set in platform code if supported
    LIMITED_GOP_SIZE | PARALLEL_ENCODING | NO_RC_BUF_LIMIT
  };

  #ifdef SUNSHINE_BUILD_VULKAN
  encoder_t vulkan {
    "vulkan"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VULKAN,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VULKAN,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vulkan_init_avcodec_hardware_input_buffer
    ),
    {
      // AV1
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
        {"async_depth"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vulkan"s,
      {},  // capabilities
    },
    {
      // HEVC
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
        {"async_depth"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vulkan"s,
      {},  // capabilities
    },
    {
      // H.264
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
        {"async_depth"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vulkan"s,
      {},  // capabilities
    },
    LIMITED_GOP_SIZE | PARALLEL_ENCODING
  };
  #endif  // SUNSHINE_BUILD_VULKAN
#endif  // linux

#ifdef __APPLE__
  /**
   * @brief VideoToolbox, driven directly through VTCompressionSession.
   */
  encoder_t videotoolbox {
    "videotoolbox"sv,
    std::make_unique<encoder_platform_formats_vt>(
      platf::mem_type_e::videotoolbox,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::unknown,
      platf::pix_fmt_e::unknown
    ),
    {
      // Common options
      {},
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vt"s,
      {},  // capabilities
    },
    {
      // Common options
      {},
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vt"s,
      {},  // capabilities
    },
    {
      // Common options
      {},
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vt"s,
      {},  // capabilities
    },
    PARALLEL_ENCODING
  };
#endif

  static const std::vector encoders {
#ifndef __APPLE__
    &nvenc,
#endif
#ifdef _WIN32
    &quicksync,
    &amdvce,
    &mediafoundation,
#endif
#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  #ifdef SUNSHINE_BUILD_VULKAN
    &vulkan,
  #endif
    &vaapi,
#endif
#ifdef __APPLE__
    &videotoolbox,
#endif
    &software
  };

  static encoder_t *chosen_encoder;
  int active_hevc_mode;  ///< HEVC mode selected by the most recent encoder probe.
  int active_av1_mode;  ///< AV1 mode selected by the most recent encoder probe.
  uint32_t active_pyrowave_profiles;  ///< PyroWave profile bits validated on the selected capture backend.
  bool last_encoder_probe_supported_ref_frames_invalidation = false;  ///< Whether the last probe found reference-frame invalidation support.
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};  ///< YUV444 support discovered for each probed codec.
  std::array<bool, 3> last_encoder_probe_supported_dynamic_range_for_codec = {};  ///< 10-bit 4:2:0 support discovered for each probed codec.
  std::array<bool, 3> last_encoder_probe_supported_dynamic_range_yuv444_for_codec = {};  ///< 10-bit YUV444 support discovered for each probed codec.

  /**
   * @brief Recreate a display capture object after a capture failure.
   *
   * @param disp Display connection or display handle.
   * @param type Protocol, message, or resource type selector.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   */
  void reset_display(std::shared_ptr<platf::display_t> &disp, const platf::mem_type_e &type, const std::string &display_name, const config_t &config) {
    // We try this twice, in case we still get an error on reinitialization
    for (int x = 0; x < 2; ++x) {
      disp.reset();
      disp = platf::display(type, display_name, config);
      if (disp) {
        break;
      }

      // The capture code depends on us to sleep between failures
      std::this_thread::sleep_for(200ms);
    }
  }

  /**
   * @brief Update the list of display names before or during a stream.
   * @details This will attempt to keep `current_display_index` pointing at the same display.
   * @param dev_type The encoder device type used for display lookup.
   * @param display_names The list of display names to repopulate.
   * @param current_display_index The current display index or -1 if not yet known.
   */
  void refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index) {
    // It is possible that the output name may be empty even if it wasn't before (device disconnected) or vice-versa
    const auto output_name {display_device::map_output_name(config::video.output_name)};
    std::string current_display_name;

    // If we have a current display index, let's start with that
    if (current_display_index >= 0 && current_display_index < display_names.size()) {
      current_display_name = display_names.at(current_display_index);
    }

    // Refresh the display names
    auto old_display_names = std::move(display_names);
    display_names = platf::display_names(dev_type);

    // If we now have no displays, let's put the old display array back and fail
    if (display_names.empty() && !old_display_names.empty()) {
      BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
      display_names = std::move(old_display_names);
      return;
    } else if (display_names.empty()) {
      display_names.emplace_back(output_name);
    }

    // We now have a new display name list, so reset the index back to 0
    current_display_index = 0;

    // If we had a name previously, let's try to find it in the new list
    if (!current_display_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == current_display_name) {
          current_display_index = x;
          return;
        }
      }

      // The old display was removed, so we'll start back at the first display again
      BOOST_LOG(warning) << "Previous active display ["sv << current_display_name << "] is no longer present"sv;
    } else {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == output_name) {
          current_display_index = x;
          return;
        }
      }
    }
  }

  /**
   * @brief Run the shared display capture thread for asynchronous encoding.
   * @details Capture callbacks share this thread's session queues and reusable image pool.
   *
   * @param capture_ctx_queue Capture context queue.
   * @param display_wp Weak pointer holder for the active display.
   * @param reinit_event Signal raised while the display is being reinitialized.
   * @param encoder Selected encoder.
   */
  void captureThread(
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
    sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
    safe::signal_t &reinit_event,
    const encoder_t &encoder
  ) {
    std::vector<capture_ctx_t> capture_ctxs;

    auto fg = util::fail_guard([&capture_ctx_queue, &capture_ctxs]() {
      capture_ctx_queue->stop();

      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
        capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
        capture_ctx.images->stop();
      }
    });

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    // Wait for the initial capture context or a request to stop the queue
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));

    // Get all the monitor names now, rather than at boot, to
    // get the most up-to-date list available monitors
    std::vector<std::string> display_names;
    int display_p = -1;
    refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);
    auto disp = platf::display(encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
    if (!disp) {
      return;
    }
    display_wp = disp;

    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);

    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    const std::chrono::seconds trim_timeot = 3s;
    auto trim_imgs = [&imgs, &imgs_used_timestamps, &trim_timeot]() {
      // count allocated and used within current pool
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // remember the timestamp of currently used count
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // decide whether to trim allocated unused above the currently used count
      // based on last used timestamp and universal timeout
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // trim allocated unused above the newly decided trim target
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
        // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();
            to_trim -= 1;
            if (to_trim == 0) {
              break;
            }
          }
        }
        // forget timestamps that no longer relevant
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    auto pull_free_image_callback = [&capture_ctx_queue, &imgs, &disp, &trim_imgs](std::shared_ptr<platf::img_t> &img_out) {
      img_out.reset();
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          // trim allocated but unused portion of the pool based on timeouts
          trim_imgs();
          img_out->frame_timestamp.reset();
          return true;
        } else {
          // sleep and retry if image pool is full
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };

    // Capture takes place on this thread
    platf::set_thread_name("video::capture");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&capture_ctxs, &capture_ctx_queue, &switch_display_event, &artificial_reinit](std::shared_ptr<platf::img_t> &&img, bool frame_captured) {
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }

          if (frame_captured) {
            capture_ctx->images->raise(img);
          }

          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        while (capture_ctx_queue->peek()) {
          capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
        }

        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);

      if (artificial_reinit && status != platf::capture_e::capture_error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit:
          {
            reinit_event.raise(true);

            // Some classes of images contain references to the display --> display won't delete unless img is deleted
            for (auto &img : imgs) {
              img.reset();
            }

            // display_wp is modified in this thread only
            // Wait for the other shared_ptr's of display to be destroyed.
            // New displays will only be created in this thread.
            while (display_wp->use_count() != 1) {
              // Free images that weren't consumed by the encoders. These can reference the display and prevent
              // the ref count from reaching 1. We do this here rather than on the encoder thread to avoid race
              // conditions where the encoding loop might free a good frame after reinitializing if we capture
              // a new frame here before the encoder has finished reinitializing.
              KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
                if (!capture_ctx->images->running()) {
                  capture_ctx = capture_ctxs.erase(capture_ctx);
                  continue;
                }

                while (capture_ctx->images->try_pop()) {
                }

                ++capture_ctx;
              });

              std::this_thread::sleep_for(20ms);
            }

            while (capture_ctx_queue->running()) {
              // Release the display before reenumerating displays, since some capture backends
              // only support a single display session per device/application.
              disp.reset();

              // Refresh display names since a display removal might have caused the reinitialization
              refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

              // Process any pending display switch with the new list of displays
              if (switch_display_event->peek()) {
                display_p = std::clamp(*switch_display_event->pop(), 0, static_cast<int>(display_names.size()) - 1);
              }

              // reset_display() will sleep between retries
              reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
              if (disp) {
                break;
              }
            }
            if (!disp) {
              return;
            }

            display_wp = disp;

            reinit_event.reset();
            continue;
          }
        case platf::capture_e::capture_error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
          return;
      }
    }
  }

  void remember_frame_timestamp(frame_timestamp_queue_t &timestamps, int64_t frame_nr, const std::optional<std::chrono::steady_clock::time_point> &frame_timestamp) {
    if (!frame_timestamp) {
      return;
    }

    timestamps.emplace_back(frame_nr, *frame_timestamp);
    if (timestamps.size() > 128) {
      timestamps.pop_front();
    }
  }

  std::optional<std::chrono::steady_clock::time_point> match_frame_timestamp(frame_timestamp_queue_t &timestamps, int64_t pts) {
    while (!timestamps.empty() && timestamps.front().first < pts) {
      timestamps.pop_front();
    }

    if (!timestamps.empty() && timestamps.front().first == pts) {
      auto timestamp = timestamps.front().second;
      timestamps.pop_front();
      return timestamp;
    }

    return std::nullopt;
  }

  /**
   * @brief Drain encoded packets from an FFmpeg encoder session.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active FFmpeg encoder session.
   * @param packets Output queue that receives encoded packets.
   * @param channel_data Stream session that receives each encoded packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when encoding or packetization fails.
   */
  int encode_avcodec(int64_t frame_nr, avcodec_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto &frame = session.device->frame;
    frame->pts = frame_nr;

    remember_frame_timestamp(session.frame_timestamps, frame_nr, frame_timestamp);

    auto &ctx = session.avcodec_ctx;

    auto &sps = session.sps;
    auto &vps = session.vps;

    // send the frame to the encoder
    auto ret = avcodec_send_frame(ctx.get(), frame);
    if (ret < 0) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> err_str {0};
      BOOST_LOG(error) << "Could not send a frame for encoding: "sv << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, ret);

      return -1;
    }

    while (ret >= 0) {
      auto packet = std::make_unique<packet_raw_avcodec>();
      auto av_packet = packet.get()->av_packet;

      ret = avcodec_receive_packet(ctx.get(), av_packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
      } else if (ret < 0) {
        return ret;
      }

      if (av_packet->flags & AV_PKT_FLAG_KEY) {
        BOOST_LOG(debug) << "Frame "sv << frame_nr << ": IDR Keyframe (AV_FRAME_FLAG_KEY)"sv;
      }

      if ((frame->flags & AV_FRAME_FLAG_KEY) && !(av_packet->flags & AV_PKT_FLAG_KEY)) {
        BOOST_LOG(error) << "Encoder did not produce IDR frame when requested!"sv;
      }

      if (session.inject) {
        if (session.inject == 1) {
          auto h264 = cbs::make_sps_h264(ctx.get(), av_packet);

          sps = std::move(h264.sps);
        } else {
          auto hevc = cbs::make_sps_hevc(ctx.get(), av_packet);

          sps = std::move(hevc.sps);
          vps = std::move(hevc.vps);

          session.replacements.emplace_back(
            std::string_view(reinterpret_cast<const char *>(std::begin(vps.old)), vps.old.size()),
            std::string_view(reinterpret_cast<const char *>(std::begin(vps._new)), vps._new.size())
          );
        }

        session.inject = 0;

        session.replacements.emplace_back(
          std::string_view(reinterpret_cast<const char *>(std::begin(sps.old)), sps.old.size()),
          std::string_view(reinterpret_cast<const char *>(std::begin(sps._new)), sps._new.size())
        );
      }

      packet->frame_timestamp = match_frame_timestamp(session.frame_timestamps, av_packet->pts);

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      packets->raise(std::move(packet));
    }

    return 0;
  }

  /**
   * @brief Encode one frame through NVENC and queue the resulting packet.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active NVENC encoder session.
   * @param packets Output queue that receives the encoded packet.
   * @param channel_data Stream session that receives the encoded packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when NVENC encoding fails.
   */
  int encode_nvenc(int64_t frame_nr, nvenc_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "NvENC returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "NvENC frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  /**
   * @brief Encode one frame through VideoToolbox and queue the resulting packet.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active VideoToolbox encoder session.
   * @param packets Output queue that receives the encoded packet.
   * @param channel_data Stream session that receives the encoded packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when VideoToolbox encoding fails.
   */
  int encode_vt(int64_t frame_nr, vt_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "VideoToolbox returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "VideoToolbox frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  /**
   * @brief Encode one captured frame and queue packets for transmission.
   *
   * @param frame_nr Frame nr.
   * @param session Active streaming or pairing session for the request.
   * @param packets Packets queued or emitted by the stream.
   * @param channel_data Channel data.
   * @param frame_timestamp Frame timestamp.
   * @return 0 when the frame is encoded and queued; nonzero on encoder failure.
   */
  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp);
    } else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);
    } else if (auto vt_session = dynamic_cast<vt_encode_session_t *>(&session)) {
      return encode_vt(frame_nr, *vt_session, packets, channel_data, frame_timestamp);
    } else if (auto pyrowave_session = dynamic_cast<pyrowave::session_t *>(&session)) {
      auto packet = pyrowave_session->encode(frame_nr);
      if (!packet) {
        return -1;
      }
      packet->channel_data = channel_data;
      packet->frame_timestamp = frame_timestamp;
      packets->raise(std::move(packet));
      return 0;
    }

    return -1;
  }

  /**
   * @brief Create an AVCodec encode session.
   *
   * @param disp Display being encoded.
   * @param encoder Selected encoder.
   * @param config Video configuration.
   * @param width Encoded frame width.
   * @param height Encoded frame height.
   * @param encode_device AVCodec encode device.
   * @return AVCodec encode session, or nullptr on failure.
   */
  std::unique_ptr<avcodec_encode_session_t> make_avcodec_encode_session(
    platf::display_t *disp,
    const encoder_t &encoder,
    const config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::avcodec_encode_device_t> encode_device
  ) {
    auto platform_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
    if (!platform_formats) {
      return nullptr;
    }

    bool hardware = platform_formats->avcodec_base_dev_type != AV_HWDEVICE_TYPE_NONE;

    auto &video_format = encoder.codec_from_config(config);
    if (!video_format[encoder_t::PASSED] || !disp->is_codec_supported(video_format.name, config)) {
      BOOST_LOG(error) << encoder.name << ": "sv << video_format.name << " mode not supported"sv;
      return nullptr;
    }

    if (config.chromaSamplingType == 1) {
      if (!video_format[encoder_t::YUV444]) {
        BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 not supported"sv;
        return nullptr;
      }

      if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 dynamic range not supported"sv;
        return nullptr;
      }

    } else {
      if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(error) << video_format.name << ": dynamic range not supported"sv;
        return nullptr;
      }
    }

    auto codec = avcodec_find_encoder_by_name(video_format.name.c_str());
    if (!codec) {
      BOOST_LOG(error) << "Couldn't open ["sv << video_format.name << ']';

      return nullptr;
    }

    auto colorspace = encode_device->colorspace;
    auto sw_fmt = (colorspace.bit_depth == 8 && config.chromaSamplingType == 0)  ? platform_formats->avcodec_pix_fmt_8bit :
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 1)  ? platform_formats->avcodec_pix_fmt_yuv444_8bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 0) ? platform_formats->avcodec_pix_fmt_10bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 1) ? platform_formats->avcodec_pix_fmt_yuv444_10bit :
                                                                                   AV_PIX_FMT_NONE;

    // Allow up to 1 retry to apply the set of fallback options.
    //
    // Note: If we later end up needing multiple sets of
    // fallback options, we may need to allow more retries
    // to try applying each set.
    avcodec_ctx_t ctx;
    for (int retries = 0; retries < 2; retries++) {
      ctx.reset(avcodec_alloc_context3(codec));
      ctx->width = config.width;
      ctx->height = config.height;
      const AVRational fps = video::framerate_to_rational(config);
      ctx->framerate = fps;
      ctx->time_base = AVRational {fps.den, fps.num};

      switch (config.videoFormat) {
        case 0:
          // 10-bit h264 encoding is not supported by our streaming protocol
          assert(!config.dynamicRange);
          ctx->profile = select_h264_profile(video_format.name, config, config::video.amd.amd_coder);
          break;

        case 1:
          if (config.chromaSamplingType == 1) {
            // HEVC uses the same RExt profile for both 8 and 10 bit YUV 4:4:4 encoding
            ctx->profile = AV_PROFILE_HEVC_REXT;
          } else {
            ctx->profile = config.dynamicRange ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
          }
          break;

        case 2:
          // AV1 supports both 8 and 10 bit encoding with the same Main profile
          // but YUV 4:4:4 sampling requires High profile
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_AV1_HIGH : AV_PROFILE_AV1_MAIN;
          break;
      }

      // B-frames delay decoder output, so never use them
      ctx->max_b_frames = 0;

      // Use an infinite GOP length since I-frames are generated on demand
      // Exception: encoders with FIXED_GOP_SIZE flag don't support on-demand IDR
      if (encoder.flags & FIXED_GOP_SIZE) {
        // Fixed GOP for encoders that don't support on-demand IDR (e.g. Media Foundation)
        ctx->gop_size = 120;  // ~2 seconds at 60 FPS - larger to reduce oversized IDR frame frequency
        ctx->keyint_min = 120;
      } else {
        ctx->gop_size = encoder.flags & LIMITED_GOP_SIZE ?
                          std::numeric_limits<std::int16_t>::max() :
                          std::numeric_limits<int>::max();
        ctx->keyint_min = std::numeric_limits<int>::max();
      }

      // Some client decoders have limits on the number of reference frames
      if (config.numRefFrames) {
        if (video_format[encoder_t::REF_FRAMES_RESTRICT]) {
          ctx->refs = config.numRefFrames;
        } else {
          BOOST_LOG(warning) << "Client requested reference frame limit, but encoder doesn't support it!"sv;
        }
      }

      // We forcefully reset the flags to avoid clash on reuse of AVCodecContext
      ctx->flags = 0;
      ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;

      ctx->flags2 |= AV_CODEC_FLAG2_FAST;

      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

      ctx->color_range = avcodec_colorspace.range;
      ctx->color_primaries = avcodec_colorspace.primaries;
      ctx->color_trc = avcodec_colorspace.transfer_function;
      ctx->colorspace = avcodec_colorspace.matrix;

      // Used by cbs::make_sps_hevc
      ctx->sw_pix_fmt = sw_fmt;

      if (hardware) {
        avcodec_buffer_t encoding_stream_context;

        ctx->pix_fmt = platform_formats->avcodec_dev_pix_fmt;

        // Create the base hwdevice context
        auto buf_or_error = platform_formats->init_avcodec_hardware_input_buffer(encode_device.get());
        if (buf_or_error.has_right()) {
          return nullptr;
        }
        encoding_stream_context = std::move(buf_or_error.left());

        // If this encoder requires derivation from the base, derive the desired type
        if (platform_formats->avcodec_derived_dev_type != AV_HWDEVICE_TYPE_NONE) {
          avcodec_buffer_t derived_context;

          // Allow the hwdevice to prepare for this type of context to be derived
          if (encode_device->prepare_to_derive_context(platform_formats->avcodec_derived_dev_type)) {
            return nullptr;
          }

          auto err = av_hwdevice_ctx_create_derived(&derived_context, platform_formats->avcodec_derived_dev_type, encoding_stream_context.get(), 0);
          if (err) {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> err_str {0};
            BOOST_LOG(error) << "Failed to derive device context: "sv << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, err);

            return nullptr;
          }

          encoding_stream_context = std::move(derived_context);
        }

        // Initialize avcodec hardware frames
        {
          avcodec_buffer_t frame_ref {av_hwframe_ctx_alloc(encoding_stream_context.get())};

          auto frame_ctx = (AVHWFramesContext *) frame_ref->data;
          frame_ctx->format = ctx->pix_fmt;
          frame_ctx->sw_format = sw_fmt;
          frame_ctx->height = ctx->height;
          frame_ctx->width = ctx->width;
          frame_ctx->initial_pool_size = 0;

          // Allow the hwdevice to modify hwframe context parameters
          encode_device->init_hwframes(frame_ctx);

          if (auto err = av_hwframe_ctx_init(frame_ref.get()); err < 0) {
            return nullptr;
          }

          ctx->hw_frames_ctx = av_buffer_ref(frame_ref.get());
        }

        ctx->slices = config.slicesPerFrame;
      } else /* software */ {
        ctx->pix_fmt = sw_fmt;

        // Clients will request for the fewest slices per frame to get the
        // most efficient encode, but we may want to provide more slices than
        // requested to ensure we have enough parallelism for good performance.
        ctx->slices = std::max(config.slicesPerFrame, config::video.min_threads);
      }

      if (encoder.flags & SINGLE_SLICE_ONLY) {
        ctx->slices = 1;
      }

      ctx->thread_type = FF_THREAD_SLICE;
      ctx->thread_count = ctx->slices;

      AVDictionary *options {nullptr};
      auto handle_option = [&options, &config](const encoder_t::option_t &option) {
        std::visit(
          util::overloaded {
            [&options, &option](int v) {
              av_dict_set_int(&options, option.name.c_str(), v, 0);
            },
            [&options, &option](int *v) {
              av_dict_set_int(&options, option.name.c_str(), *v, 0);
            },
            [&options, &option](std::optional<int> *v) {
              if (v->has_value()) {
                av_dict_set_int(&options, option.name.c_str(), **v, 0);
              }
            },
            [&options, &option](const std::function<int()> &v) {
              av_dict_set_int(&options, option.name.c_str(), v(), 0);
            },
            [&options, &option](const std::string &v) {
              av_dict_set(&options, option.name.c_str(), v.c_str(), 0);
            },
            [&options, &option](std::string *v) {
              if (!v->empty()) {
                av_dict_set(&options, option.name.c_str(), v->c_str(), 0);
              }
            },
            [&options, &option, &config](const std::function<const std::string(const config_t &cfg)> &v) {
              av_dict_set(&options, option.name.c_str(), v(config).c_str(), 0);
            }
          },
          option.value
        );
      };

      // Apply common options, then format-specific overrides
      for (auto &option : video_format.common_options) {
        handle_option(option);
      }
      for (auto &option : (config.dynamicRange ? video_format.hdr_options : video_format.sdr_options)) {
        handle_option(option);
      }
      if (config.chromaSamplingType == 1) {
        for (auto &option : (config.dynamicRange ? video_format.hdr444_options : video_format.sdr444_options)) {
          handle_option(option);
        }
      }
      if (retries > 0) {
        for (auto &option : video_format.fallback_options) {
          handle_option(option);
        }
      }

      auto bitrate = ((config::video.max_bitrate > 0) ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate) * 1000;
      BOOST_LOG(info) << "Streaming bitrate is " << bitrate;
      ctx->rc_max_rate = bitrate;
      ctx->bit_rate = bitrate;

      if (encoder.flags & CBR_WITH_VBR) {
        // Ensure rc_max_bitrate != bit_rate to force VBR mode
        ctx->bit_rate--;
      } else {
        ctx->rc_min_rate = bitrate;
      }

      if (encoder.flags & RELAXED_COMPLIANCE) {
        ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
      }

      if (!(encoder.flags & NO_RC_BUF_LIMIT)) {
        if (!hardware && (ctx->slices > 1 || config.videoFormat == 1)) {
          // Use a larger rc_buffer_size for software encoding when slices are enabled,
          // because libx264 can severely degrade quality if the buffer is too small.
          // libx265 encounters this issue more frequently, so always scale the
          // buffer by 1.5x for software HEVC encoding.
          ctx->rc_buffer_size = bitrate / ((config.framerate * 10) / 15);
        } else {
          ctx->rc_buffer_size = bitrate / config.framerate;

#ifndef __APPLE__
          if (encoder.name == "nvenc" && config::video.nv_legacy.vbv_percentage_increase > 0) {
            ctx->rc_buffer_size += ctx->rc_buffer_size * config::video.nv_legacy.vbv_percentage_increase / 100;
          }
#endif
        }
      }

      // Allow the encoding device a final opportunity to set/unset or override any options
      encode_device->init_codec_options(ctx.get(), &options);

      if (auto status = avcodec_open2(ctx.get(), codec, &options)) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> err_str {0};

        if (!video_format.fallback_options.empty() && retries == 0) {
          BOOST_LOG(info)
            << "Retrying with fallback configuration options for ["sv << video_format.name << "] after error: "sv
            << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, status);

          continue;
        } else {
          BOOST_LOG(error)
            << "Could not open codec ["sv
            << video_format.name << "]: "sv
            << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, status);

          return nullptr;
        }
      }

      // Successfully opened the codec
      break;
    }

    avcodec_frame_t frame {av_frame_alloc()};
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->chroma_location = ctx->chroma_sample_location;

    // Attach HDR metadata to the AVFrame
    if (colorspace_is_hdr(colorspace)) {
      SS_HDR_METADATA hdr_metadata;
      if (disp->get_hdr_metadata(hdr_metadata)) {
        auto mdm = av_mastering_display_metadata_create_side_data(frame.get());

        mdm->display_primaries[0][0] = av_make_q(hdr_metadata.displayPrimaries[0].x, 50000);
        mdm->display_primaries[0][1] = av_make_q(hdr_metadata.displayPrimaries[0].y, 50000);
        mdm->display_primaries[1][0] = av_make_q(hdr_metadata.displayPrimaries[1].x, 50000);
        mdm->display_primaries[1][1] = av_make_q(hdr_metadata.displayPrimaries[1].y, 50000);
        mdm->display_primaries[2][0] = av_make_q(hdr_metadata.displayPrimaries[2].x, 50000);
        mdm->display_primaries[2][1] = av_make_q(hdr_metadata.displayPrimaries[2].y, 50000);

        mdm->white_point[0] = av_make_q(hdr_metadata.whitePoint.x, 50000);
        mdm->white_point[1] = av_make_q(hdr_metadata.whitePoint.y, 50000);

        mdm->min_luminance = av_make_q(hdr_metadata.minDisplayLuminance, 10000);
        mdm->max_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);

        mdm->has_luminance = hdr_metadata.maxDisplayLuminance != 0 ? 1 : 0;
        mdm->has_primaries = hdr_metadata.displayPrimaries[0].x != 0 ? 1 : 0;

        if (hdr_metadata.maxContentLightLevel != 0 || hdr_metadata.maxFrameAverageLightLevel != 0) {
          auto clm = av_content_light_metadata_create_side_data(frame.get());

          clm->MaxCLL = hdr_metadata.maxContentLightLevel;
          clm->MaxFALL = hdr_metadata.maxFrameAverageLightLevel;
        }
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }

    std::unique_ptr<platf::avcodec_encode_device_t> encode_device_final;

    if (!encode_device->hardware) {
      auto software_encode_device = std::make_unique<avcodec_software_encode_device_t>();

      if (software_encode_device->init(width, height, frame.get(), sw_fmt, hardware)) {
        return nullptr;
      }
      software_encode_device->colorspace = colorspace;

      encode_device_final = std::move(software_encode_device);
    } else {
      encode_device_final = std::move(encode_device);
    }

    if (encode_device_final->set_frame(frame.release(), ctx->hw_frames_ctx)) {
      return nullptr;
    }

    encode_device_final->apply_colorspace();

    auto session = std::make_unique<avcodec_encode_session_t>(
      std::move(ctx),
      std::move(encode_device_final),

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - static_cast<int>(video_format[encoder_t::VUI_PARAMETERS])) * (1 + config.videoFormat) : 0
    );

    return session;
  }

  /**
   * @brief Create NVENC encode session.
   *
   * @param client_config Client stream configuration negotiated for this session.
   * @param encode_device Encode device.
   * @return Constructed NVENC encode session object.
   */
  std::unique_ptr<nvenc_encode_session_t> make_nvenc_encode_session(const config_t &client_config, std::unique_ptr<platf::nvenc_encode_device_t> encode_device) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace)) {
      return nullptr;
    }

    return std::make_unique<nvenc_encode_session_t>(std::move(encode_device));
  }

  /**
   * @brief Create native VideoToolbox encode session.
   *
   * @param client_config Client stream configuration negotiated for this session.
   * @param encode_device Encode device.
   * @return Constructed VideoToolbox encode session object.
   */
  std::unique_ptr<vt_encode_session_t> make_vt_encode_session(const config_t &client_config, std::unique_ptr<platf::vt_encode_device_t> encode_device) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace)) {
      return nullptr;
    }

    return std::make_unique<vt_encode_session_t>(std::move(encode_device));
  }

  /**
   * @brief Prepare the existing capture converter for PyroWave without opening an FFmpeg encoder.
   * @param encoder Encoder descriptor selecting the capture memory type.
   * @param config Negotiated PyroWave configuration.
   * @param width Captured image width.
   * @param height Captured image height.
   * @param device Platform color-conversion device.
   * @param api PyroWave C API entry points.
   * @return PyroWave session, or null if conversion or encoding is unavailable.
   */
  std::unique_ptr<encode_session_t> make_pyrowave_encode_session(const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::avcodec_encode_device_t> device, const pyrowave::api_t &api = {}) {
    if (!pyrowave::valid_config(config) || !device || !encoder.platform_formats || width <= 0 || height <= 0) {
      return nullptr;
    }
    const auto colorspace = device->colorspace;
    // The software converter accepts 8-bit capture pixels and cannot preserve HDR.
    if (config.dynamicRange && !device->hardware) {
      return nullptr;
    }

    auto formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
#ifdef _WIN32
    // Native NVENC, QSV, and AMF all use the same D3D11 capture converter.
    // PyroWave needs the base D3D11 frame, without a derived QSV context.
    static const encoder_platform_formats_avcodec d3d_formats {
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX,
      AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer
    };
    if (encoder.platform_formats->dev_type == platf::mem_type_e::dxgi) {
      formats = &d3d_formats;
    }
#endif
    if (!formats) {
      return nullptr;
    }
    const auto sw_format = pyrowave::capture_format(*encoder.platform_formats, config);
    if (sw_format == AV_PIX_FMT_NONE) {
      return nullptr;
    }
    avcodec_frame_t frame {av_frame_alloc()};
    if (!frame) {
      return nullptr;
    }
    frame->width = config.width;
    frame->height = config.height;
    const auto metadata = avcodec_colorspace_from_sunshine_colorspace(colorspace);
    frame->color_range = metadata.range;
    frame->color_primaries = metadata.primaries;
    frame->color_trc = metadata.transfer_function;
    frame->colorspace = metadata.matrix;

    avcodec_buffer_t frames;
    if (device->hardware) {
      if (!formats->init_avcodec_hardware_input_buffer) {
        return nullptr;
      }
      auto context = formats->init_avcodec_hardware_input_buffer(device.get());
      if (context.has_right()) {
        return nullptr;
      }
      frames.reset(av_hwframe_ctx_alloc(context.left().get()));
      if (!frames) {
        return nullptr;
      }
      auto hw_frames = reinterpret_cast<AVHWFramesContext *>(frames->data);
      hw_frames->format = formats->avcodec_dev_pix_fmt;
      hw_frames->sw_format = sw_format;
      hw_frames->width = config.width;
      hw_frames->height = config.height;
      device->init_hwframes(hw_frames);
#ifdef SUNSHINE_BUILD_VULKAN
      if (hw_frames->format == AV_PIX_FMT_VULKAN) {
        auto vk_frames = static_cast<AVVulkanFramesContext *>(hw_frames->hwctx);
        vk_frames->usage = static_cast<VkImageUsageFlagBits>((vk_frames->usage & ~VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
      }
#endif
      if (av_hwframe_ctx_init(frames.get()) < 0) {
        return nullptr;
      }
      frame->format = hw_frames->format;
    } else {
      frame->format = sw_format;
      auto converter = std::make_unique<avcodec_software_encode_device_t>();
      if (converter->init(width, height, frame.get(), sw_format, false)) {
        return nullptr;
      }
      converter->colorspace = colorspace;
      device = std::move(converter);
    }
    if (device->set_frame(frame.release(), frames.get())) {
      return nullptr;
    }
    device->apply_colorspace();
    return pyrowave::make_session(config, std::move(device), api, std::move(frames));
  }

  /**
   * @brief Create encode session.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param encode_device Encode device.
   * @return Constructed encode session object.
   */
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device) {
    if (config.videoFormat == pyrowave::video_format) {
      return make_pyrowave_encode_session(encoder, config, width, height, boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device)));
    }
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, config, width, height, std::move(avcodec_encode_device));
    } else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));
    } else if (dynamic_cast<platf::vt_encode_device_t *>(encode_device.get())) {
      auto vt_encode_device = boost::dynamic_pointer_cast<platf::vt_encode_device_t>(std::move(encode_device));
      return make_vt_encode_session(config, std::move(vt_encode_device));
    }

    return nullptr;
  }

  /**
   * @brief Run one encode loop for a display capture stream.
   *
   * @param frame_nr Frame counter updated as frames are encoded.
   * @param mail Session mail bus.
   * @param images Captured image event source.
   * @param config Video configuration.
   * @param disp Display being encoded.
   * @param encode_device Platform encode device.
   * @param reinit_event Signal raised while the encoder/display is reinitializing.
   * @param encoder Selected encoder.
   * @param channel_data Stream session carried with each encoded packet.
   */
  void encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    stream::session_t *channel_data
  ) {
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return;
    }

    // As a workaround for NVENC hangs and to generally speed up encoder reinit,
    // we will complete the encoder teardown in a separate thread if supported.
    // This will move expensive processing off the encoder thread to allow us
    // to restart encoding as soon as possible. For cases where the NVENC driver
    // hang occurs, this thread may probably never exit, but it will allow
    // streaming to continue without requiring a full restart of Sunshine.
    auto fail_guard = util::fail_guard([&encoder, &session] {
      if (encoder.flags & ASYNC_TEARDOWN) {
        std::jthread encoder_teardown_thread {[session = std::move(session)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          session.reset();
          BOOST_LOG(info) << "Async encoder teardown complete";
        }};
        encoder_teardown_thread.detach();
      }
    });

    // set max frame time based on client-requested target framerate.
    double minimum_fps_target = (config::video.minimum_fps_target > 0.0) ? config::video.minimum_fps_target : (config.framerate / 2);
    std::chrono::duration<double, std::milli> max_frametime {1000.0 / minimum_fps_target};
    BOOST_LOG(info) << "Minimum FPS target set to ~"sv << minimum_fps_target << "fps ("sv << max_frametime.count() << "ms)"sv;

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);

    {
      // Load a dummy image into the AVFrame to ensure we have something to encode
      // even if we timeout waiting on the first frame. This is a relatively large
      // allocation which can be freed immediately after convert(), so we do this
      // in a separate scope.
      auto dummy_img = disp->alloc_img();
      if (!dummy_img || disp->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
        return;
      }
    }

    while (true) {
      bool requested_idr_frame = false;

      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_events->peek()) {
        requested_idr_frame = true;
        idr_events->pop();
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

      // Encode at a minimum FPS to avoid image quality issues with static content
      if (!requested_idr_frame || images->peek()) {
        if (auto img = images->pop(max_frametime)) {
          frame_timestamp = img->frame_timestamp;
          if (session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            return;
          }
        } else if (!images->running()) {
          break;
        }
      }

      // Break out of the encoding loop if any of the following are true:
      // a) The stream is ending
      // b) Sunshine is quitting
      // c) The capture side is waiting to reinit and we've encoded at least one frame
      //
      // If we have to reinit before we have received any captured frames, we will encode
      // the blank dummy frame just to let Moonlight know that we're alive.
      //
      // Ensure that this check occurs as close as possible to the encode call to prevent packets
      // in flight after encoder teardown.
      if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
        break;
      }

      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp)) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        return;
      }

      session->request_normal_frame();

      // While streaming check to see if the mouse is present and enable Mouse Keys to force the cursor to appear
      // This is useful for KVM switch scenarios where mouse may disappear during streaming
      platf::enable_mouse_keys();
    }
  }

  input::touch_port_t make_port(platf::display_t *display, const config_t &config) {
    float wd = display->width;
    float hd = display->height;

    float wt = config.width;
    float ht = config.height;

    auto scalar = std::fminf(wt / wd, ht / hd);

    // we initialize scalar_tpcoords and logical dimensions to default values in case they are not set (non-KMS)
    float scalar_tpcoords = 1.0f;
    int display_env_logical_width = 0;
    int display_env_logical_height = 0;
    if (display->logical_width > 0 && display->logical_height > 0 && display->env_logical_width > 0 && display->env_logical_height > 0) {
      scalar_tpcoords = touch_port_scale(wd, hd, display->logical_width, display->logical_height, display->transform);
      display_env_logical_width = display->env_logical_width;
      display_env_logical_height = display->env_logical_height;
    }

    auto w2 = scalar * wd;
    auto h2 = scalar * hd;

    auto offsetX = (config.width - w2) * 0.5f;
    auto offsetY = (config.height - h2) * 0.5f;

    return input::touch_port_t {
      {
        display->offset_x,
        display->offset_y,
        config.width,
        config.height,
        display->logical_width,
        display->logical_height,
        display->env_offset_x,
        display->env_offset_y,
        display->output_name,
        display->transform,
      },
      display->env_width,
      display->env_height,
      offsetX,
      offsetY,
      1.0f / scalar,
      scalar_tpcoords,
      display_env_logical_width,
      display_env_logical_height
    };
  }

  /**
   * @brief Create encode device.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @return Constructed encode device object.
   */
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &disp, const encoder_t &encoder, const config_t &config) {
    std::unique_ptr<platf::encode_device_t> result;

    auto colorspace = colorspace_from_client_config(config, disp.is_hdr());

    platf::pix_fmt_e pix_fmt;
    if (config.videoFormat == pyrowave::video_format) {
      pix_fmt = map_pix_fmt(pyrowave::capture_format(*encoder.platform_formats, config));
      if (pix_fmt == platf::pix_fmt_e::unknown) {
        return {};
      }
    } else if (config.chromaSamplingType == 1) {
      // YUV 4:4:4
      if (!(encoder.flags & YUV444_SUPPORT)) {
        // Encoder can't support YUV 4:4:4 regardless of hardware capabilities
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv444_10bit :
                  encoder.platform_formats->pix_fmt_yuv444_8bit;
    } else {
      // YUV 4:2:0
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_10bit :
                  encoder.platform_formats->pix_fmt_8bit;
    }

    {
      auto encoder_name = config.videoFormat == pyrowave::video_format ? "pyrowave"s : encoder.codec_from_config(config).name;

      BOOST_LOG(info) << "Creating encoder " << logging::bracket(encoder_name);

      BOOST_LOG(info) << "Color coding: " << colorspace_to_string(colorspace.colorspace);
      BOOST_LOG(info) << "Color depth: " << colorspace.bit_depth << "-bit";
      BOOST_LOG(info) << "Color range: " << (colorspace.full_range ? "JPEG" : "MPEG");
    }

    if (config.videoFormat == pyrowave::video_format || dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get())) {
      result = disp.make_avcodec_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_nvenc *>(encoder.platform_formats.get())) {
      result = disp.make_nvenc_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_vt *>(encoder.platform_formats.get())) {
      result = disp.make_vt_encode_device(pix_fmt);
    }

    if (result) {
      result->colorspace = colorspace;
    }

    return result;
  }

  /**
   * @brief Create synced session.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param img Image or frame object to read from or populate.
   * @param ctx Native context object used by the operation or callback.
   * @return Constructed synced session object.
   */
  std::optional<sync_session_t> make_synced_session(platf::display_t *disp, const encoder_t &encoder, platf::img_t &img, sync_session_ctx_t &ctx) {
    sync_session_t encode_session;

    encode_session.ctx = &ctx;

    auto encode_device = make_encode_device(*disp, encoder, ctx.config);
    if (!encode_device) {
      return std::nullopt;
    }

    // absolute mouse coordinates require that the dimensions of the screen are known
    ctx.touch_port_events->raise(make_port(disp, ctx.config));

    // Update client with our current HDR display state
    auto hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (colorspace_is_hdr(encode_device->colorspace)) {
      if (disp->get_hdr_metadata(hdr_info->metadata)) {
        hdr_info->enabled = true;
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }
    ctx.hdr_events->raise(std::move(hdr_info));

    auto session = make_encode_session(disp, encoder, ctx.config, img.width, img.height, std::move(encode_device));
    if (!session) {
      return std::nullopt;
    }

    // Load the initial image to prepare for encoding
    if (session->convert(img)) {
      BOOST_LOG(error) << "Could not convert initial image"sv;
      return std::nullopt;
    }

    encode_session.session = std::move(session);

    return encode_session;
  }

  /**
   * @brief Run synchronized capture and encoding.
   *
   * @param synced_session_ctxs Active synchronized session contexts.
   * @param encode_session_ctx_queue Pending synchronized session context queue.
   * @param display_names Cached display names.
   * @param display_p Active display index.
   * @return Encoder loop result.
   */
  encode_e encode_run_sync(
    std::vector<std::unique_ptr<sync_session_ctx_t>> &synced_session_ctxs,
    encode_session_ctx_queue_t &encode_session_ctx_queue,
    std::vector<std::string> &display_names,
    int &display_p
  ) {
    const auto &encoder = *chosen_encoder;

    std::shared_ptr<platf::display_t> disp;

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    if (synced_session_ctxs.empty()) {
      auto ctx = encode_session_ctx_queue.pop();
      if (!ctx) {
        return encode_e::ok;
      }

      synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*ctx)));
    }

    while (encode_session_ctx_queue.running()) {
      // Refresh display names since a display removal might have caused the reinitialization
      refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

      // Process any pending display switch with the new list of displays
      if (switch_display_event->peek()) {
        display_p = std::clamp(*switch_display_event->pop(), 0, static_cast<int>(display_names.size()) - 1);
      }

      // reset_display() will sleep between retries
      reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], synced_session_ctxs.front()->config);
      if (disp) {
        break;
      }
    }

    if (!disp) {
      return encode_e::capture_error;
    }

    auto img = disp->alloc_img();
    if (!img || disp->dummy_img(img.get())) {
      return encode_e::capture_error;
    }

    std::vector<sync_session_t> synced_sessions;
    for (auto &ctx : synced_session_ctxs) {
      auto synced_session = make_synced_session(disp.get(), encoder, *img, *ctx);
      if (!synced_session) {
        return encode_e::capture_error;
      }

      synced_sessions.emplace_back(std::move(*synced_session));
    }

    auto ec = platf::capture_e::ok;
    while (encode_session_ctx_queue.running()) {
      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&captured_image, bool frame_captured) {
        while (encode_session_ctx_queue.peek()) {
          auto encode_session_ctx = encode_session_ctx_queue.pop();
          if (!encode_session_ctx) {
            return false;
          }

          synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*encode_session_ctx)));

          auto encode_session = make_synced_session(disp.get(), encoder, *captured_image, *synced_session_ctxs.back());
          if (!encode_session) {
            ec = platf::capture_e::capture_error;
            return false;
          }

          synced_sessions.emplace_back(std::move(*encode_session));
        }

        KITTY_WHILE_LOOP(auto pos = std::begin(synced_sessions), pos != std::end(synced_sessions), {
          auto ctx = pos->ctx;
          if (ctx->shutdown_event->peek()) {
            // Let waiting thread know it can delete shutdown_event
            ctx->join_event->raise(true);

            pos = synced_sessions.erase(pos);
            synced_session_ctxs.erase(std::find_if(std::begin(synced_session_ctxs), std::end(synced_session_ctxs), [&ctx_p = ctx](auto &ctx) {
              return ctx.get() == ctx_p;
            }));

            if (synced_sessions.empty()) {
              return false;
            }

            continue;
          }

          if (ctx->idr_events->peek()) {
            pos->session->request_idr_frame();
            ctx->idr_events->pop();
          }

          if (frame_captured && pos->session->convert(*captured_image)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
          if (captured_image) {
            frame_timestamp = captured_image->frame_timestamp;
          }

          if (encode(ctx->frame_nr++, *pos->session, ctx->packets, ctx->channel_data, frame_timestamp)) {
            BOOST_LOG(error) << "Could not encode video packet"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          pos->session->request_normal_frame();

          ++pos;
        })

        if (switch_display_event->peek()) {
          ec = platf::capture_e::reinit;
          return false;
        }

        return true;
      };

      auto pull_free_image_callback = [&img](std::shared_ptr<platf::img_t> &img_out) {
        img_out = img;
        img_out->frame_timestamp.reset();
        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);
      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::capture_error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return ec != platf::capture_e::ok ? ec : status;
      }
    }

    return encode_e::ok;
  }

  /**
   * @brief Run synchronous capture and encode work on the capture thread.
   */
  void captureThreadSync() {
    auto ref = capture_thread_sync.ref();

    std::vector<std::unique_ptr<sync_session_ctx_t>> synced_session_ctxs;

    auto &ctx = ref->encode_session_ctx_queue;
    auto lg = util::fail_guard([&ctx, &synced_session_ctxs]() {
      ctx.stop();

      for (const auto &session_ctx : synced_session_ctxs) {
        session_ctx->shutdown_event->raise(true);
        session_ctx->join_event->raise(true);
      }

      for (auto &pending_ctx : ctx.unsafe()) {
        pending_ctx.shutdown_event->raise(true);
        pending_ctx.join_event->raise(true);
      }
    });

    // Encoding and capture takes place on this thread
    platf::set_thread_name("video::capture_sync");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    std::vector<std::string> display_names;
    int display_p = -1;
    while (encode_run_sync(synced_session_ctxs, ctx, display_names, display_p) == encode_e::reinit) {}
  }

  /**
   * @brief Capture and encode video using the asynchronous capture thread.
   *
   * @param mail Session mail bus.
   * @param config Video configuration.
   * @param channel_data Stream session carried with each encoded packet.
   */
  void capture_async(
    safe::mail_t mail,
    config_t &config,
    stream::session_t *channel_data
  ) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<img_event_t::element_type>();
    auto lg = util::fail_guard([&images, &shutdown_event]() {
      images->stop();
      shutdown_event->raise(true);
    });

    auto ref = capture_thread_async.ref();
    if (!ref) {
      return;
    }

    ref->capture_ctx_queue->raise(capture_ctx_t {images, config});

    if (!ref->capture_ctx_queue->running()) {
      return;
    }

    int frame_nr = 1;

    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (ref->reinit_event.peek()) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto display_lock = ref->display_wp.lock();
        if (ref->display_wp->expired()) {
          continue;
        }

        display = ref->display_wp->lock();
      }

      auto &encoder = *chosen_encoder;

      auto encode_device = make_encode_device(*display, encoder, config);
      if (!encode_device) {
        return;
      }

      // absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      // Update client with our current HDR display state
      auto hdr_info = std::make_unique<hdr_info_raw_t>(false);
      if (colorspace_is_hdr(encode_device->colorspace)) {
        if (display->get_hdr_metadata(hdr_info->metadata)) {
          hdr_info->enabled = true;
        } else {
          BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
        }
      }
      hdr_event->raise(std::move(hdr_info));

      encode_run(
        frame_nr,
        mail,
        images,
        config,
        display,
        std::move(encode_device),
        ref->reinit_event,
        *ref->encoder_p,
        channel_data
      );
    }
  }

  /**
   * @brief Capture and encode video for a streaming session.
   *
   * @param mail Session mail bus.
   * @param config Client-requested video configuration, normalized before capture begins.
   * @param channel_data Stream session carried with each encoded packet.
   */
  void capture(
    safe::mail_t mail,
    config_t config,
    stream::session_t *channel_data
  ) {
    config = resolve_dynamic_range(*chosen_encoder, config);

    auto idr_events = mail->event<bool>(mail::idr);

    idr_events->raise(true);
    if (chosen_encoder->flags & PARALLEL_ENCODING) {
      capture_async(std::move(mail), config, channel_data);
    } else {
      safe::signal_t join_event;
      auto ref = capture_thread_sync.ref();
      ref->encode_session_ctx_queue.raise(sync_session_ctx_t {
        &join_event,
        mail->event<bool>(mail::shutdown),
        mail::man->queue<packet_t>(mail::video_packets),
        std::move(idr_events),
        mail->event<hdr_info_t>(mail::hdr),
        mail->event<input::touch_port_t>(mail::touch_port),
        config,
        1,
        channel_data,
      });

      // Wait for join signal
      join_event.view();
    }
  }

  /**
   * @brief Enumerates supported validate flag options.
   */
  enum validate_flag_e {
    VUI_PARAMS = 0x01,  ///< VUI parameters
  };

  /**
   * @brief Validate config before it is used.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @return 0 when the selected encoder/device accepts the configuration; nonzero otherwise.
   */
  int validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config) {
    auto encode_device = make_encode_device(*disp, encoder, config);
    if (!encode_device) {
      return -1;
    }

    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return -1;
    }

    {
      // Image buffers are large, so we use a separate scope to free it immediately after convert()
      auto img = disp->alloc_img();
      if (!img || disp->dummy_img(img.get()) || session->convert(*img)) {
        return -1;
      }
    }

    session->request_idr_frame();

    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    while (!packets->peek()) {
      if (encode(1, *session, packets, nullptr, {})) {
        return -1;
      }
    }

    auto packet = packets->pop();
    if (!packet->is_idr()) {
      BOOST_LOG(error) << "First packet type is not an IDR frame"sv;

      return -1;
    }

    int flag = 0;

    // This check only applies for H.264 and HEVC
    if (config.videoFormat <= 1) {
      if (auto packet_avcodec = dynamic_cast<packet_raw_avcodec *>(packet.get())) {
        if (cbs::validate_sps(packet_avcodec->av_packet, config.videoFormat ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264)) {
          flag |= VUI_PARAMS;
        }
      } else {
        // Don't check it for non-avcodec encoders.
        flag |= VUI_PARAMS;
      }
    }

    return flag;
  }

  /**
   * @brief Validate encoder before it is used.
   */
  bool validate_encoder(encoder_t &encoder, bool expect_failure) {
    const auto output_name {display_device::map_output_name(config::video.output_name)};
    std::shared_ptr<platf::display_t> disp;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] failed"sv;
    });

    auto test_hevc = active_hevc_mode >= 2 || (active_hevc_mode == 0 && !(encoder.flags & H264_ONLY));
    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));

    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();

    // First, test encoder viability
    config_t config_max_ref_frames {1920, 1080, 60, 6000, 1000, 1, 1, 1, 0, 0, 0, 0};
    config_t config_autoselect {1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 0, 0};

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, output_name, config_autoselect);
    if (!disp) {
      return false;
    }
    if (!disp->is_codec_supported(encoder.h264.name, config_autoselect)) {
      fg.disable();
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] is not supported on this GPU"sv;
      return false;
    }

    // If we're expecting failure, use the autoselect ref config first since that will always succeed
    // if the encoder is available.
    auto max_ref_frames_h264 = expect_failure ? -1 : validate_config(disp, encoder, config_max_ref_frames);
    auto autoselect_h264 = max_ref_frames_h264 >= 0 ? max_ref_frames_h264 : validate_config(disp, encoder, config_autoselect);
    if (autoselect_h264 < 0) {
      return false;
    } else if (expect_failure) {
      // We expected failure, but actually succeeded. Do the max_ref_frames probe we skipped.
      max_ref_frames_h264 = validate_config(disp, encoder, config_max_ref_frames);
    }

    std::vector<std::pair<validate_flag_e, encoder_t::flag_e>> packet_deficiencies {
      {VUI_PARAMS, encoder_t::VUI_PARAMETERS},
    };

    for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
      encoder.h264[encoder_flag] = (max_ref_frames_h264 & validate_flag && autoselect_h264 & validate_flag);
    }

    encoder.h264[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_h264 >= 0;
    encoder.h264[encoder_t::PASSED] = true;

    if (test_hevc) {
      config_max_ref_frames.videoFormat = 1;
      config_autoselect.videoFormat = 1;

      if (disp->is_codec_supported(encoder.hevc.name, config_autoselect)) {
        auto max_ref_frames_hevc = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // HEVC to also succeed with max ref frames specified if HEVC is supported.
        auto autoselect_hevc = (max_ref_frames_hevc >= 0 || max_ref_frames_h264 >= 0) ?
                                 max_ref_frames_hevc :
                                 validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.hevc[encoder_flag] = (max_ref_frames_hevc & validate_flag && autoselect_hevc & validate_flag);
        }

        encoder.hevc[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_hevc >= 0;
        encoder.hevc[encoder_t::PASSED] = max_ref_frames_hevc >= 0 || autoselect_hevc >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.hevc.name << "] is not supported on this GPU"sv;
        encoder.hevc.capabilities.reset();
      }
    } else {
      // Clear all cap bits for HEVC if we didn't probe it
      encoder.hevc.capabilities.reset();
    }

    if (test_av1) {
      config_max_ref_frames.videoFormat = 2;
      config_autoselect.videoFormat = 2;

      if (disp->is_codec_supported(encoder.av1.name, config_autoselect)) {
        auto max_ref_frames_av1 = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // AV1 to also succeed with max ref frames specified if AV1 is supported.
        auto autoselect_av1 = (max_ref_frames_av1 >= 0 || max_ref_frames_h264 >= 0) ?
                                max_ref_frames_av1 :
                                validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.av1[encoder_flag] = (max_ref_frames_av1 & validate_flag && autoselect_av1 & validate_flag);
        }

        encoder.av1[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_av1 >= 0;
        encoder.av1[encoder_t::PASSED] = max_ref_frames_av1 >= 0 || autoselect_av1 >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.av1.name << "] is not supported on this GPU"sv;
        encoder.av1.capabilities.reset();
      }
    } else {
      // Clear all cap bits for AV1 if we didn't probe it
      encoder.av1.capabilities.reset();
    }

    // Test HDR and YUV444 support
    {
      auto test_yuv444 = [&](auto &flag_map, auto video_format) {
        const config_t config = {1920, 1080, 60, 6000, 1000, 1, 0, 1, video_format, 0, 1, 0};

        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return;
        }
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if ((encoder.flags & YUV444_SUPPORT) && disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::YUV444] = true;
        } else {
          flag_map[encoder_t::YUV444] = false;
        }
      };

      auto test_yuv420_hdr = [&](auto &flag_map, auto video_format) {
        const config_t config = {1920, 1080, 60, 6000, 1000, 1, 0, 3, video_format, 1, 0, 0};

        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return;
        }
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if (disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE] = true;
        } else {
          flag_map[encoder_t::DYNAMIC_RANGE] = false;
        }
      };

      auto test_yuv444_hdr = [&](auto &flag_map, auto video_format) {
        const config_t config = {1920, 1080, 60, 6000, 1000, 1, 0, 3, video_format, 1, 1, 0};

        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return;
        }
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if ((encoder.flags & YUV444_SUPPORT) && disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE_YUV444] = true;
        } else {
          flag_map[encoder_t::DYNAMIC_RANGE_YUV444] = false;
        }
      };

      test_yuv444(encoder.h264, 0);
      // HDR is not supported with H.264. Don't bother even trying it.
      encoder.h264[encoder_t::DYNAMIC_RANGE] = false;
      encoder.h264[encoder_t::DYNAMIC_RANGE_YUV444] = false;

      test_yuv444(encoder.hevc, 1);
      test_yuv420_hdr(encoder.hevc, 1);
      test_yuv444_hdr(encoder.hevc, 1);
      test_yuv444(encoder.av1, 2);
      test_yuv420_hdr(encoder.av1, 2);
      test_yuv444_hdr(encoder.av1, 2);
    }

    encoder.h264[encoder_t::VUI_PARAMETERS] = encoder.h264[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];
    encoder.hevc[encoder_t::VUI_PARAMETERS] = encoder.hevc[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];

    if (!encoder.h264[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": h264 missing sps->vui parameters"sv;
    }
    if (encoder.hevc[encoder_t::PASSED] && !encoder.hevc[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": hevc missing sps->vui parameters"sv;
    }

    fg.disable();
    return true;
  }

  void reset_encoder_probe_results() {
    last_encoder_probe_supported_ref_frames_invalidation = false;
    last_encoder_probe_supported_yuv444_for_codec = {};
    last_encoder_probe_supported_dynamic_range_for_codec = {};
    last_encoder_probe_supported_dynamic_range_yuv444_for_codec = {};
  }

  void record_encoder_probe_results(const encoder_t &encoder) {
    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                       encoder.av1[encoder_t::YUV444];
    last_encoder_probe_supported_dynamic_range_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                              encoder.h264[encoder_t::DYNAMIC_RANGE];
    last_encoder_probe_supported_dynamic_range_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                              encoder.hevc[encoder_t::DYNAMIC_RANGE];
    last_encoder_probe_supported_dynamic_range_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                              encoder.av1[encoder_t::DYNAMIC_RANGE];
    last_encoder_probe_supported_dynamic_range_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                                     encoder.h264[encoder_t::DYNAMIC_RANGE_YUV444];
    last_encoder_probe_supported_dynamic_range_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                                     encoder.hevc[encoder_t::DYNAMIC_RANGE_YUV444];
    last_encoder_probe_supported_dynamic_range_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                                     encoder.av1[encoder_t::DYNAMIC_RANGE_YUV444];
  }

  /**
   * @brief Probe each PyroWave color depth and chroma profile using the selected capture backend.
   * @param encoder Encoder descriptor selecting the capture backend.
   */
  void probe_pyrowave(const encoder_t &encoder) {
    active_pyrowave_profiles = 0;
    if (!config::video.pyrowave) {
      return;
    }
    const auto output_name = display_device::map_output_name(config::video.output_name);
    std::shared_ptr<platf::display_t> display;
    active_pyrowave_profiles = pyrowave::probe_profiles([&](const config_t &probe) {
      reset_display(display, encoder.platform_formats->dev_type, output_name, probe);
      return display && validate_config(display, encoder, probe) >= 0;
    });
    if (!active_pyrowave_profiles) {
      BOOST_LOG(warning) << "PyroWave is unavailable on the selected capture backend";
    }
  }

  int probe_encoders() {
    if (!allow_encoder_probing(display_device::enumerate_devices())) {
      // Error already logged
      return -1;
    }

    auto encoder_list = encoders;

    // If we already have a good encoder, check to see if another probe is required
    if (chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      probe_pyrowave(*chosen_encoder);
      return 0;
    }

    // Restart encoder selection
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;
    active_hevc_mode = config::video.hevc_mode;
    active_av1_mode = config::video.av1_mode;
    active_pyrowave_profiles = 0;
    reset_encoder_probe_results();

    auto adjust_encoder_constraints_hevc = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_hevc_mode == 5 && !encoder->hevc[encoder_t::DYNAMIC_RANGE] && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 Rext10_444 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 4 && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Rext10_444 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 2 && !encoder->hevc[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC on this system"sv;
        active_hevc_mode = 0;
      }
    };

    auto adjust_encoder_constraints_av1 = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_av1_mode == 5 && !encoder->av1[encoder_t::DYNAMIC_RANGE] && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 Rext10_444 on this system"sv;
        active_av1_mode = 0;
      } else if (active_av1_mode == 4 && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Rext10_444 on this system"sv;
        active_av1_mode = 0;
      } else if (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 on this system"sv;
        active_av1_mode = 0;
      } else if (active_av1_mode == 2 && !encoder->av1[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 on this system"sv;
        active_av1_mode = 0;
      }
    };

    if (!config::video.encoder.empty()) {
      // If there is a specific encoder specified, use it if it passes validation
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        if (encoder->name == config::video.encoder) {
          // Remove the encoder from the list entirely if it fails validation
          if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
            pos = encoder_list.erase(pos);
            break;
          }

          // We will return an encoder here even if it fails one of the codec requirements specified by the user
          adjust_encoder_constraints_hevc(encoder);
          adjust_encoder_constraints_av1(encoder);

          chosen_encoder = encoder;
          break;
        }

        pos++;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder matching ["sv << config::video.encoder << ']';
      }
    }

    BOOST_LOG(info) << "// Testing for available encoders, this may generate errors. You can safely ignore those errors. //"sv;

    // If we haven't found an encoder yet, but we want one with specific codec support, search for that now.
    if (chosen_encoder == nullptr && (active_hevc_mode >= 2 || active_av1_mode >= 2)) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // Remove the encoder from the list entirely if it fails validation
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // Skip it if it doesn't support the specified codec at all
        if ((active_hevc_mode >= 2 && !encoder->hevc[encoder_t::PASSED]) || (active_av1_mode >= 2 && !encoder->av1[encoder_t::PASSED])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 5 && !encoder->hevc[encoder_t::DYNAMIC_RANGE] && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) || (active_av1_mode == 5 && !encoder->av1[encoder_t::DYNAMIC_RANGE] && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 4 && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) || (active_av1_mode == 4 && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) || (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE])) {
          pos++;
          continue;
        }

        chosen_encoder = encoder;
        break;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder that meets HEVC/AV1 requirements"sv;
      }
    }

    // If no encoder was specified or the specified encoder was unusable, keep trying
    // the remaining encoders until we find one that passes validation.
    if (chosen_encoder == nullptr) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // If we've used a previous encoder and it's not this one, we expect this encoder to
        // fail to validate. It will use a slightly different order of checks to more quickly
        // eliminate failing encoders.
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // We will return an encoder here even if it fails one of the codec requirements specified by the user
        adjust_encoder_constraints_hevc(encoder);
        adjust_encoder_constraints_av1(encoder);

        chosen_encoder = encoder;
        break;
      });
    }

    if (chosen_encoder == nullptr) {
      const auto output_name {display_device::map_output_name(config::video.output_name)};
      BOOST_LOG(fatal) << "Unable to find display or encoder during startup."sv;
      if (!config::video.adapter_name.empty() || !output_name.empty()) {
        BOOST_LOG(fatal) << "Please ensure your manually chosen GPU and monitor are connected and powered on."sv;
      } else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      return -1;
    }

    BOOST_LOG(info);
    BOOST_LOG(info) << "// Ignore any errors mentioned above, they are not relevant. //"sv;
    BOOST_LOG(info);

    auto &encoder = *chosen_encoder;

    probe_pyrowave(encoder);

    record_encoder_probe_results(encoder);

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = static_cast<encoder_t::flag_e>(x);
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = static_cast<encoder_t::flag_e>(x);
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

    if (encoder.av1[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  av1 ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = static_cast<encoder_t::flag_e>(x);
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.av1[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found AV1 encoder: "sv << encoder.av1.name << " ["sv << encoder.name << ']';
    }

    // 2 - passed
    // 3 - HDR yuv420
    // 4 - HDR yuv444
    // 5 - HDR yuv420 & HDR yuv444

    if (active_hevc_mode == 0) {
      active_hevc_mode = 1;
      if (encoder.hevc[encoder_t::PASSED]) {
        active_hevc_mode = 2;
        if (encoder.hevc[encoder_t::DYNAMIC_RANGE]) {
          active_hevc_mode += 1;
        }
        if (encoder.hevc[encoder_t::DYNAMIC_RANGE_YUV444]) {
          active_hevc_mode += 2;
        }
      }
      BOOST_LOG(debug) << "ENCODER STATUS ACTIVE_HEVC_MODE: "sv << active_hevc_mode;
    }

    if (active_av1_mode == 0) {
      active_av1_mode = 1;
      if (encoder.av1[encoder_t::PASSED]) {
        active_av1_mode = 2;
        if (encoder.av1[encoder_t::DYNAMIC_RANGE]) {
          active_av1_mode += 1;
        }
        if (encoder.av1[encoder_t::DYNAMIC_RANGE_YUV444]) {
          active_av1_mode += 2;
        }
      }
      BOOST_LOG(debug) << "ENCODER STATUS ACTIVE_AV1_MODE: "sv << active_av1_mode;
    }

    return 0;
  }

  /**
   * @brief Initialize AVCodec hardware input buffers for VA-API.
   */
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    // If an egl hwdevice
    if (encode_device->init_hw_device) {
      if (encode_device->init_hw_device(encode_device, &hw_device_buf)) {
        return -1;
      }

      return hw_device_buf;
    }

    auto render_device = platf::resolve_render_device();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VAAPI, render_device.empty() ? nullptr : render_device.c_str(), nullptr, 0);
    if (status < 0) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> string;
      BOOST_LOG(error) << "Failed to create a VAAPI device: "sv << av_make_error_string(string.data(), AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef SUNSHINE_BUILD_VULKAN
  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    if (encode_device && encode_device->init_hw_device) {
      if (encode_device->init_hw_device(encode_device, &hw_device_buf)) {
        return -1;
      }
      return hw_device_buf;
    }

    // Try render device path first, auto-detecting the GPU with a connected display
    auto render_device = platf::resolve_render_device();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, render_device.c_str(), nullptr, 0);
    if (status >= 0) {
      BOOST_LOG(info) << "Using Vulkan device: "sv << render_device;
      return hw_device_buf;
    }

    // Fallback: try device indices for multi-GPU systems
    const std::array<const char *, 4> devices = {"1", "0", "2", "3"};
    for (auto device : devices) {
      status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, device, nullptr, 0);
      if (status >= 0) {
        BOOST_LOG(info) << "Using Vulkan device index: "sv << device;
        return hw_device_buf;
      }
    }

    BOOST_LOG(error) << "Failed to create a Vulkan device"sv;
    return -1;
  }
#endif

  /**
   * @brief Initialize AVCodec hardware input buffers for CUDA.
   */
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1 /* AV_CUDA_USE_PRIMARY_CONTEXT */);
    if (status < 0) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> string;
      BOOST_LOG(error) << "Failed to create a CUDA device: "sv << av_make_error_string(string.data(), AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef _WIN32
}

/**
 * @brief No-op lock callback used when FFmpeg requires a D3D11VA lock function.
 */
void do_nothing(void *) {  // NOSONAR(cpp:S5008): FFmpeg's D3D11VA lock callbacks require a void pointer for user data.
}

namespace video {
  /**
   * @brief Create an FFmpeg D3D11VA hardware device from Sunshine's DXGI device.
   */
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t ctx_buf {av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)};
    auto ctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) ctx_buf->data)->hwctx;

    std::fill_n((std::uint8_t *) ctx, sizeof(AVD3D11VADeviceContext), 0);

    auto device = encode_device->d3d11_device;

    device->AddRef();
    ctx->device = device;

    ctx->lock_ctx = (void *) 1;
    ctx->lock = do_nothing;
    ctx->unlock = do_nothing;

    auto err = av_hwdevice_ctx_init(ctx_buf.get());
    if (err) {
      std::array<char, AV_ERROR_MAX_STRING_SIZE> err_str {0};
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str.data(), AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return ctx_buf;
  }
#endif

  /**
   * @brief Start capture async.
   */
  int start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.encoder_p = chosen_encoder;
    capture_thread_ctx.reinit_event.reset();

    capture_thread_ctx.capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::jthread {
      captureThread,
      capture_thread_ctx.capture_ctx_queue,
      std::ref(capture_thread_ctx.display_wp),
      std::ref(capture_thread_ctx.reinit_event),
      std::ref(*capture_thread_ctx.encoder_p)
    };

    return 0;
  }

  /**
   * @brief Stop capture async processing.
   */
  void end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.capture_ctx_queue->stop();

    capture_thread_ctx.capture_thread.join();
  }

  /**
   * @brief Start capture sync.
   * @details The shared lifetime callback supplies unused state; the thread accesses the shared capture queue directly.
   */
  int start_capture_sync(capture_thread_sync_ctx_t &) {
    std::jthread {&captureThreadSync}.detach();
    return 0;
  }

  /**
   * @brief Leave synchronous capture cleanup to the capture thread.
   */
  void end_capture_sync(capture_thread_sync_ctx_t &) {
    // The capture thread stops its queue and signals its sessions when it exits.
  }

  /**
   * @brief Map an FFmpeg hardware device type to Sunshine's capture memory type.
   */
  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {
    using enum platf::mem_type_e;
    switch (type) {
      case AV_HWDEVICE_TYPE_D3D11VA:
        return dxgi;
      case AV_HWDEVICE_TYPE_VAAPI:
        return vaapi;
#ifdef SUNSHINE_BUILD_VULKAN
      case AV_HWDEVICE_TYPE_VULKAN:
        return vulkan;
#endif
      case AV_HWDEVICE_TYPE_CUDA:
        return cuda;
      case AV_HWDEVICE_TYPE_NONE:
        return system;
      default:
        return unknown;
    }

    return unknown;
  }

  /**
   * @brief Map an FFmpeg pixel format to Sunshine's pixel format.
   */
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt) {
    using enum platf::pix_fmt_e;
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return ayuv;
      case AV_PIX_FMT_XV30:
        return y410;
      case AV_PIX_FMT_YUV420P10:
        return yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return yuv420p;
      case AV_PIX_FMT_NV12:
        return nv12;
      case AV_PIX_FMT_P010:
        return p010;
      case AV_PIX_FMT_YUV444P:
        return yuv444p;
      case AV_PIX_FMT_YUV444P16:
        return yuv444p16;
      default:
        return unknown;
    }

    return unknown;
  }

}  // namespace video
