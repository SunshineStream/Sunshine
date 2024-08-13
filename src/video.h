/**
 * @file src/video.h
 * @brief Declarations for video.
 */
#pragma once

#include "input/processor.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "video_colorspace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct AVPacket;
namespace video {

  platf::mem_type_e
  map_base_dev_type(AVHWDeviceType type);
  platf::pix_fmt_e
  map_pix_fmt(AVPixelFormat fmt);

  void
  free_ctx(AVCodecContext *ctx);
  void
  free_frame(AVFrame *frame);
  void
  free_buffer(AVBufferRef *ref);

  using avcodec_ctx_t = util::safe_ptr<AVCodecContext, free_ctx>;
  using avcodec_frame_t = util::safe_ptr<AVFrame, free_frame>;
  using avcodec_buffer_t = util::safe_ptr<AVBufferRef, free_buffer>;
  using sws_t = util::safe_ptr<SwsContext, sws_freeContext>;
  using img_event_t = std::shared_ptr<safe::event_t<std::shared_ptr<platf::img_t>>>;

  struct encoder_platform_formats_t {
    virtual ~encoder_platform_formats_t() = default;
    platf::mem_type_e dev_type;
    platf::pix_fmt_e pix_fmt_8bit, pix_fmt_10bit;
  };

  struct encoder_platform_formats_avcodec: encoder_platform_formats_t {
    using init_buffer_function_t = std::function<util::Either<avcodec_buffer_t, int>(platf::avcodec_encode_device_t *)>;

    encoder_platform_formats_avcodec(
      const AVHWDeviceType &avcodec_base_dev_type,
      const AVHWDeviceType &avcodec_derived_dev_type,
      const AVPixelFormat &avcodec_dev_pix_fmt,
      const AVPixelFormat &avcodec_pix_fmt_8bit,
      const AVPixelFormat &avcodec_pix_fmt_10bit,
      const init_buffer_function_t &init_avcodec_hardware_input_buffer_function):
        avcodec_base_dev_type { avcodec_base_dev_type },
        avcodec_derived_dev_type { avcodec_derived_dev_type },
        avcodec_dev_pix_fmt { avcodec_dev_pix_fmt },
        avcodec_pix_fmt_8bit { avcodec_pix_fmt_8bit },
        avcodec_pix_fmt_10bit { avcodec_pix_fmt_10bit },
        init_avcodec_hardware_input_buffer { init_avcodec_hardware_input_buffer_function } {
      dev_type = map_base_dev_type(avcodec_base_dev_type);
      pix_fmt_8bit = map_pix_fmt(avcodec_pix_fmt_8bit);
      pix_fmt_10bit = map_pix_fmt(avcodec_pix_fmt_10bit);
    }

    AVHWDeviceType avcodec_base_dev_type, avcodec_derived_dev_type;
    AVPixelFormat avcodec_dev_pix_fmt;
    AVPixelFormat avcodec_pix_fmt_8bit, avcodec_pix_fmt_10bit;

    init_buffer_function_t init_avcodec_hardware_input_buffer;
  };

  struct encoder_platform_formats_nvenc: encoder_platform_formats_t {
    encoder_platform_formats_nvenc(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
    }
  };

  struct encoder_t {
    std::string_view name;
    enum flag_e {
      PASSED,  ///< Indicates the encoder is supported.
      REF_FRAMES_RESTRICT,  ///< Set maximum reference frames.
      CBR,  ///< Some encoders don't support CBR, if not supported attempt constant quantization parameter instead.
      DYNAMIC_RANGE,  ///< HDR support.
      VUI_PARAMETERS,  ///< AMD encoder with VAAPI doesn't add VUI parameters to SPS.
      MAX_FLAGS  ///< Maximum number of flags.
    };

    static std::string_view
    from_flag(flag_e flag) {
#define _CONVERT(x) \
  case flag_e::x:   \
    return std::string_view(#x)
      switch (flag) {
        _CONVERT(PASSED);
        _CONVERT(REF_FRAMES_RESTRICT);
        _CONVERT(CBR);
        _CONVERT(DYNAMIC_RANGE);
        _CONVERT(VUI_PARAMETERS);
        _CONVERT(MAX_FLAGS);
      }
#undef _CONVERT

      return { "unknown" };
    }

    struct option_t {
      KITTY_DEFAULT_CONSTR_MOVE(option_t)
      option_t(const option_t &) = default;

      std::string name;
      std::variant<int, int *, std::optional<int> *, std::function<int()>, std::string, std::string *> value;

      option_t(std::string &&name, decltype(value) &&value):
          name { std::move(name) }, value { std::move(value) } {}
    };

    const std::unique_ptr<const encoder_platform_formats_t> platform_formats;

    struct codec_t {
      std::vector<option_t> common_options;
      std::vector<option_t> sdr_options;
      std::vector<option_t> hdr_options;
      std::vector<option_t> fallback_options;

      // QP option to set in the case that CBR/VBR is not supported
      // by the encoder. If CBR/VBR is guaranteed to be supported,
      // don't specify this option to avoid wasteful encoder probing.
      std::optional<option_t> qp;

      std::string name;
      std::bitset<MAX_FLAGS> capabilities;

      bool
      operator[](flag_e flag) const {
        return capabilities[(std::size_t) flag];
      }

      std::bitset<MAX_FLAGS>::reference
      operator[](flag_e flag) {
        return capabilities[(std::size_t) flag];
      }
    } av1, hevc, h264;

    uint32_t flags;
  };

  struct encode_session_t {
    virtual ~encode_session_t() = default;

    virtual int
    convert(platf::img_t &img) = 0;

    virtual void
    request_idr_frame() = 0;

    virtual void
    request_normal_frame() = 0;

    virtual void
    invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;
  };

  // encoders
  extern encoder_t software;

#if !defined(__APPLE__)
  extern encoder_t nvenc;  // available for windows and linux
#endif

#ifdef _WIN32
  extern encoder_t amdvce;
  extern encoder_t quicksync;
#endif

#ifdef __linux__
  extern encoder_t vaapi;
#endif

#ifdef __APPLE__
  extern encoder_t videotoolbox;
#endif

  struct packet_raw_t {
    virtual ~packet_raw_t() = default;

    virtual bool
    is_idr() = 0;

    virtual int64_t
    frame_index() = 0;

    virtual uint8_t *
    data() = 0;

    virtual size_t
    data_size() = 0;

    struct replace_t {
      std::string_view old;
      std::string_view _new;

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      replace_t(std::string_view old, std::string_view _new) noexcept:
          old { std::move(old) }, _new { std::move(_new) } {}
    };

    std::vector<replace_t> *replacements = nullptr;
    void *channel_data = nullptr;
    bool after_ref_frame_invalidation = false;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
  };

  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    bool
    is_idr() override {
      return av_packet->flags & AV_PKT_FLAG_KEY;
    }

    int64_t
    frame_index() override {
      return av_packet->pts;
    }

    uint8_t *
    data() override {
      return av_packet->data;
    }

    size_t
    data_size() override {
      return av_packet->size;
    }

    AVPacket *av_packet;
  };

  struct packet_raw_generic: packet_raw_t {
    packet_raw_generic(std::vector<uint8_t> &&frame_data, int64_t frame_index, bool idr):
        frame_data { std::move(frame_data) }, index { frame_index }, idr { idr } {
    }

    bool
    is_idr() override {
      return idr;
    }

    int64_t
    frame_index() override {
      return index;
    }

    uint8_t *
    data() override {
      return frame_data.data();
    }

    size_t
    data_size() override {
      return frame_data.size();
    }

    std::vector<uint8_t> frame_data;
    int64_t index;
    bool idr;
  };

  using packet_t = std::unique_ptr<packet_raw_t>;

  struct hdr_info_raw_t {
    explicit hdr_info_raw_t(bool enabled):
        enabled { enabled }, metadata {} {};
    explicit hdr_info_raw_t(bool enabled, const SS_HDR_METADATA &metadata):
        enabled { enabled }, metadata { metadata } {};

    bool enabled;
    SS_HDR_METADATA metadata;
  };

  using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;

  /* Encoding configuration requested by remote client */
  struct config_t {
    int width;  // Video width in pixels
    int height;  // Video height in pixels
    int framerate;  // Requested framerate, used in individual frame bitrate budget calculation
    int bitrate;  // Video bitrate in kilobits (1000 bits) for requested framerate
    int slicesPerFrame;  // Number of slices per frame
    int numRefFrames;  // Max number of reference frames

    /* Requested color range and SDR encoding colorspace, HDR encoding colorspace is always BT.2020+ST2084
       Color range (encoderCscMode & 0x1) : 0 - limited, 1 - full
       SDR encoding colorspace (encoderCscMode >> 1) : 0 - BT.601, 1 - BT.709, 2 - BT.2020 */
    int encoderCscMode;

    int videoFormat;  // 0 - H.264, 1 - HEVC, 2 - AV1

    /* Encoding color depth (bit depth): 0 - 8-bit, 1 - 10-bit
       HDR encoding activates when color depth is higher than 8-bit and the display which is being captured is operating in HDR mode */
    int dynamicRange;
  };

  extern int active_hevc_mode;
  extern int active_av1_mode;
  extern bool last_encoder_probe_supported_ref_frames_invalidation;

  void
  capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data);

  bool
  validate_encoder(encoder_t &encoder, bool expect_failure);

  /**
   * @brief Probe encoders and select the preferred encoder.
   * This is called once at startup and each time a stream is launched to
   * ensure the best encoder is selected. Encoder availability can change
   * at runtime due to all sorts of things from driver updates to eGPUs.
   *
   * @warning This is only safe to call when there is no client actively streaming.
   */
  int
  probe_encoders();
}  // namespace video
