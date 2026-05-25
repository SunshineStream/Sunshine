/**
 * @file src/video.h
 * @brief Declarations for video.
 */
#pragma once

// standard includes
#include <chrono>
#include <deque>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "input.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "video_colorspace.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct AVPacket;

namespace display_device {
  struct EnumeratedDevice;
}

namespace stream {
  struct session_t;
}

namespace video {
  /**
   * @brief Video stream format identifiers used by GameStream setup.
   */
  enum video_format_e {
    format_h264 = 0,  ///< H.264 High profile.
    format_hevc = 1,  ///< HEVC Main/Main10 profile.
    format_av1 = 2,  ///< AV1 Main/Main10 profile.
    format_mpeg2 = 3,  ///< MPEG-2/H.262 video.
    format_h263p = 4,  ///< H.263+ video.
  };

  /**
   * @brief Encoding configuration requested by a remote client.
   */
  struct config_t {
    int width;  ///< Video width in pixels.
    int height;  ///< Video height in pixels.
    int framerate;  ///< Requested framerate used in the per-frame bitrate budget.
    int framerateX100;  ///< Optional NTSC-style framerate value, e.g. 59.94 as 5994.
    int bitrate;  ///< Video bitrate in kilobits for the requested framerate.
    int slicesPerFrame;  ///< Number of slices per frame.
    int numRefFrames;  ///< Maximum number of reference frames.
    int encoderCscMode;  ///< Requested color range and SDR colorspace; HDR always uses BT.2020 and ST2084.
    int videoFormat;  ///< Video codec format selected from video_format_e.
    int dynamicRange;  ///< Encoding color depth: 0 = 8-bit, 1 = 10-bit.
    int chromaSamplingType;  ///< Chroma sampling type: 0 = 4:2:0, 1 = 4:4:4.
    int enableIntraRefresh;  ///< Intra refresh setting: 0 = disabled, 1 = enabled.
  };

  /**
   * @brief Describe the streamed display, desktop bounds, and client letterboxing for absolute input.
   *
   * @param display Capture display supplying native output identity and geometry.
   * @param config Client stream dimensions.
   * @return Touch port forwarded from the video pipeline to input handling.
   */
  input::touch_port_t make_port(platf::display_t *display, const config_t &config);

  namespace amf {

    /**
     * @brief Enumerates supported coder options for the AMF encoder.
     */
    enum class coder_e : int {
      auto_ = 0,  ///< Select the coder based on the H.264 profile.
      cabac = 1,  ///< CABAC entropy coding.
      cavlc = 2,  ///< CAVLC entropy coding.
    };

  }  // namespace amf

  /**
   * @brief Select the effective FFmpeg H.264 profile for an encoder configuration.
   *
   * @param encoder_name FFmpeg encoder name selected for the stream.
   * @param config Encoding configuration requested by the remote client.
   * @param amd_coder Configured AMF entropy-coder value.
   * @return FFmpeg H.264 profile applied to the codec context.
   */
  int select_h264_profile(std::string_view encoder_name, const config_t &config, int amd_coder);

  /**
   * @brief Decide whether encoder probing may proceed with the given display devices.
   *
   * @param devices Display devices currently enumerated by libdisplaydevice.
   * @return True if there should be no issues with the probing, false if it must be prevented.
   */
  bool allow_encoder_probing(const std::vector<display_device::EnumeratedDevice> &devices);

  /**
   * @brief Map an FFmpeg hardware device type to Sunshine's memory type.
   *
   * @param type FFmpeg hardware device type reported by the encoder backend.
   * @return Sunshine memory type used by the capture and encode pipeline.
   */
  platf::mem_type_e map_base_dev_type(AVHWDeviceType type);
  /**
   * @brief Map an FFmpeg pixel format to Sunshine's pixel format enum.
   *
   * @param fmt FFmpeg pixel format to convert.
   * @return Sunshine pixel format used by display and encoder backends.
   */
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt);

  /**
   * @brief Release an FFmpeg codec context.
   *
   * @param ctx Codec context allocated by FFmpeg.
   */
  void free_ctx(AVCodecContext *ctx);
  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   *
   * @param frame Video or graphics frame being processed.
   */
  void free_frame(AVFrame *frame);
  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   *
   * @param ref FFmpeg buffer reference to unref and free.
   */
  void free_buffer(AVBufferRef *ref);

  /**
   * @brief Owning pointer for an FFmpeg codec context released with free_ctx().
   */
  using avcodec_ctx_t = util::safe_ptr<AVCodecContext, &free_ctx>;
  /**
   * @brief Owning pointer for an FFmpeg frame released with free_frame().
   */
  using avcodec_frame_t = util::safe_ptr<AVFrame, &free_frame>;
  /**
   * @brief Owning pointer for an FFmpeg buffer reference released with free_buffer().
   */
  using avcodec_buffer_t = util::safe_ptr<AVBufferRef, &free_buffer>;
  /**
   * @brief Owning pointer for an FFmpeg software-scaling context released with sws_freeContext().
   */
  using sws_t = util::safe_ptr<SwsContext, &sws_freeContext>;
  /**
   * @brief Shared event that transports captured images between capture and encode threads.
   */
  using img_event_t = std::shared_ptr<safe::event_t<std::shared_ptr<platf::img_t>>>;

  /**
   * @brief FFmpeg software encode device used when no hardware frames are required.
   */
  class avcodec_software_encode_device_t: public platf::avcodec_encode_device_t {
  public:
    /**
     * @brief Convert a captured image into the encoder input representation.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override;

    /**
     * @brief Attach frame resources used by the next conversion or encode operation.
     * @note Takes ownership of 'in_frame'.
     *
     * @param in_frame Video or graphics frame being processed.
     * @param hw_frames_ctx FFmpeg hardware frames context associated with the frame.
     * @return Status from updating frame.
     */
    int set_frame(AVFrame *in_frame, AVBufferRef *hw_frames_ctx) override;

    /**
     * @brief Apply the configured colorspace metadata to the active frame.
     */
    void apply_colorspace() override;

    /**
     * @brief Initialize FFmpeg software encoding for the requested codec.
     *
     * @param in_width In width.
     * @param in_height In height.
     * @param in_frame Video or graphics frame being processed.
     * @param format Pixel, audio, or protocol format being converted.
     * @param hardware Whether the frame is backed by hardware resources.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(int in_width, int in_height, AVFrame *in_frame, AVPixelFormat format, bool hardware);

  private:
    /**
     * @brief When preserving aspect ratio, ensure that padding is black.
     */
    void prefill();

    /**
     * @brief (Re)create the software scaler for the given source format.
     *
     * @param src_format Pixel format of the captured frames.
     * @return 0 on success; nonzero on failure.
     */
    int reinit_sws(AVPixelFormat src_format);

    // Store ownership when frame is hw_frame
    avcodec_frame_t hw_frame;  ///< Hw frame.

    avcodec_frame_t sw_frame;  ///< Sw frame.
    avcodec_frame_t sws_input_frame;  ///< Sws input frame.
    avcodec_frame_t sws_output_frame;  ///< Sws output frame.
    sws_t sws;  ///< Software scaler used when frames need CPU-side pixel conversion.
    AVPixelFormat sws_src_format {AV_PIX_FMT_BGR0};  ///< Source format the sws context was created with.

    // Offset of input image to output frame in pixels
    int offsetW;  ///< Offset w.
    int offsetH;  ///< Offset h.
  };

  /**
   * @brief Capture timestamps of in-flight frames keyed by pts, awaiting their encoded packets.
   */
  using frame_timestamp_queue_t = std::deque<std::pair<int64_t, std::chrono::steady_clock::time_point>>;

  /**
   * @brief Remember a submitted frame's capture timestamp until its encoded packet surfaces.
   *
   * Encoders may deliver packets one or more frames behind submission, so the timestamp is
   * kept keyed by pts until the corresponding packet arrives. The queue is bounded: once
   * 128 timestamps are outstanding, the oldest entry is evicted per insertion.
   * @param timestamps Queue of outstanding capture timestamps.
   * @param frame_nr Monotonic frame index assigned by the video pipeline, used as the frame pts.
   * @param frame_timestamp Capture timestamp associated with the frame, when known.
   */
  void remember_frame_timestamp(frame_timestamp_queue_t &timestamps, int64_t frame_nr, const std::optional<std::chrono::steady_clock::time_point> &frame_timestamp);

  /**
   * @brief Match an encoded packet to the capture timestamp of the frame it encodes.
   *
   * Entries for older frames whose packets never surfaced (e.g. dropped by the encoder)
   * are discarded; a packet is never paired with another frame's timestamp.
   * @param timestamps Queue of outstanding capture timestamps.
   * @param pts Presentation timestamp of the received packet.
   * @return Capture timestamp of the packet's frame, when known.
   */
  std::optional<std::chrono::steady_clock::time_point> match_frame_timestamp(frame_timestamp_queue_t &timestamps, int64_t pts);

  /**
   * @brief Pixel formats supported by one encoder backend.
   */
  struct encoder_platform_formats_t {
    virtual ~encoder_platform_formats_t() = default;
    platf::mem_type_e dev_type;  ///< Platform memory type required by this encoder.
    platf::pix_fmt_e pix_fmt_8bit;  ///< 8-bit 4:2:0 input format accepted by this encoder.
    platf::pix_fmt_e pix_fmt_10bit;  ///< 10-bit 4:2:0 input format accepted by this encoder.
    platf::pix_fmt_e pix_fmt_yuv444_8bit;  ///< 8-bit 4:4:4 input format accepted by this encoder.
    platf::pix_fmt_e pix_fmt_yuv444_10bit;  ///< 10-bit 4:4:4 input format accepted by this encoder.
  };

  /**
   * @brief AVCodec-specific pixel formats supported by a platform.
   */
  struct encoder_platform_formats_avcodec: encoder_platform_formats_t {
    /**
     * @brief Callback that prepares an FFmpeg hardware input buffer for the encode device.
     */
    using init_buffer_function_t = std::function<util::Either<avcodec_buffer_t, int>(platf::avcodec_encode_device_t *)>;

    /**
     * @brief Construct AVCodec platform format mappings.
     *
     * @param avcodec_base_dev_type Base AVCodec hardware device type.
     * @param avcodec_derived_dev_type Derived AVCodec hardware device type.
     * @param avcodec_dev_pix_fmt AVCodec device pixel format.
     * @param avcodec_pix_fmt_8bit AVCodec 8-bit pixel format.
     * @param avcodec_pix_fmt_10bit AVCodec 10-bit pixel format.
     * @param avcodec_pix_fmt_yuv444_8bit AVCodec 8-bit YUV444 pixel format.
     * @param avcodec_pix_fmt_yuv444_10bit AVCodec 10-bit YUV444 pixel format.
     * @param init_avcodec_hardware_input_buffer_function Hardware input buffer initialization callback.
     */
    encoder_platform_formats_avcodec(
      const AVHWDeviceType &avcodec_base_dev_type,
      const AVHWDeviceType &avcodec_derived_dev_type,
      const AVPixelFormat &avcodec_dev_pix_fmt,
      const AVPixelFormat &avcodec_pix_fmt_8bit,
      const AVPixelFormat &avcodec_pix_fmt_10bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_8bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_10bit,
      const init_buffer_function_t &init_avcodec_hardware_input_buffer_function
    ):
        avcodec_base_dev_type {avcodec_base_dev_type},
        avcodec_derived_dev_type {avcodec_derived_dev_type},
        avcodec_dev_pix_fmt {avcodec_dev_pix_fmt},
        avcodec_pix_fmt_8bit {avcodec_pix_fmt_8bit},
        avcodec_pix_fmt_10bit {avcodec_pix_fmt_10bit},
        avcodec_pix_fmt_yuv444_8bit {avcodec_pix_fmt_yuv444_8bit},
        avcodec_pix_fmt_yuv444_10bit {avcodec_pix_fmt_yuv444_10bit},
        init_avcodec_hardware_input_buffer {init_avcodec_hardware_input_buffer_function} {
      dev_type = map_base_dev_type(avcodec_base_dev_type);
      pix_fmt_8bit = map_pix_fmt(avcodec_pix_fmt_8bit);
      pix_fmt_10bit = map_pix_fmt(avcodec_pix_fmt_10bit);
      pix_fmt_yuv444_8bit = map_pix_fmt(avcodec_pix_fmt_yuv444_8bit);
      pix_fmt_yuv444_10bit = map_pix_fmt(avcodec_pix_fmt_yuv444_10bit);
    }

    AVHWDeviceType avcodec_base_dev_type;  ///< FFmpeg device type used to create the primary hardware context.
    AVHWDeviceType avcodec_derived_dev_type;  ///< FFmpeg device type derived from the primary hardware context.
    AVPixelFormat avcodec_dev_pix_fmt;  ///< FFmpeg hardware-device pixel format for frames handed to the encoder.
    AVPixelFormat avcodec_pix_fmt_8bit;  ///< FFmpeg 8-bit 4:2:0 software pixel format for this encoder.
    AVPixelFormat avcodec_pix_fmt_10bit;  ///< FFmpeg 10-bit 4:2:0 software pixel format for this encoder.
    AVPixelFormat avcodec_pix_fmt_yuv444_8bit;  ///< FFmpeg 8-bit 4:4:4 software pixel format for this encoder.
    AVPixelFormat avcodec_pix_fmt_yuv444_10bit;  ///< FFmpeg 10-bit 4:4:4 software pixel format for this encoder.

    init_buffer_function_t init_avcodec_hardware_input_buffer;  ///< Backend hook that allocates or imports hardware frames for FFmpeg.
  };

  /**
   * @brief NVENC-specific pixel formats supported by a platform.
   */
  struct encoder_platform_formats_nvenc: encoder_platform_formats_t {
    /**
     * @brief Construct NVENC platform format mappings.
     *
     * @param device_type Platform memory type.
     * @param format_8bit Platform 8-bit pixel format.
     * @param format_10bit Platform 10-bit pixel format.
     * @param format_yuv444_8bit Platform 8-bit YUV444 pixel format.
     * @param format_yuv444_10bit Platform 10-bit YUV444 pixel format.
     */
    encoder_platform_formats_nvenc(
      const platf::mem_type_e &device_type,
      const platf::pix_fmt_e &format_8bit,
      const platf::pix_fmt_e &format_10bit,
      const platf::pix_fmt_e &format_yuv444_8bit,
      const platf::pix_fmt_e &format_yuv444_10bit
    ) {
      encoder_platform_formats_t::dev_type = device_type;
      encoder_platform_formats_t::pix_fmt_8bit = format_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = format_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = format_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = format_yuv444_10bit;
    }
  };

  /**
   * @brief Native VideoToolbox-specific pixel formats supported by a platform.
   */
  struct encoder_platform_formats_vt: encoder_platform_formats_t {
    /**
     * @brief Construct VideoToolbox platform format mappings.
     *
     * @param dev_type Platform memory type.
     * @param pix_fmt_8bit Platform 8-bit pixel format.
     * @param pix_fmt_10bit Platform 10-bit pixel format.
     * @param pix_fmt_yuv444_8bit Platform 8-bit YUV444 pixel format.
     * @param pix_fmt_yuv444_10bit Platform 10-bit YUV444 pixel format.
     */
    encoder_platform_formats_vt(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_8bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_10bit
    ) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  /**
   * @brief Encoder name and feature flags advertised by Sunshine.
   */
  struct encoder_t {
    std::string_view name;  ///< Encoder name used in logs, configuration, and capability probes.

    /**
     * @brief Capability flags that describe which stream modes an encoder supports.
     */
    enum flag_e {
      PASSED,  ///< Indicates the encoder is supported.
      REF_FRAMES_RESTRICT,  ///< Set maximum reference frames.
      DYNAMIC_RANGE,  ///< HDR support.
      YUV444,  ///< YUV 4:4:4 support.
      DYNAMIC_RANGE_YUV444,  ///< YUV 4:4:4 HDR support.
      VUI_PARAMETERS,  ///< AMD encoder with VAAPI doesn't add VUI parameters to SPS.
      MAX_FLAGS  ///< Maximum number of flags.
    };

    /**
     * @brief Convert an encoder capability flag to its diagnostic string.
     *
     * @param flag Feature flag to name.
     * @return Stable string used in logs and probe output.
     */
    static std::string_view from_flag(flag_e flag) {
#ifndef DOXYGEN
  #define _CONVERT(x) \
    case flag_e::x: \
      return std::string_view(#x)
#endif
      switch (flag) {
        _CONVERT(PASSED);
        _CONVERT(REF_FRAMES_RESTRICT);
        _CONVERT(DYNAMIC_RANGE);
        _CONVERT(YUV444);
        _CONVERT(DYNAMIC_RANGE_YUV444);
        _CONVERT(VUI_PARAMETERS);
        _CONVERT(MAX_FLAGS);
      }
#ifndef DOXYGEN
  #undef _CONVERT
#endif

      return {"unknown"};
    }

    /**
     * @brief Runtime encoder option exposed to configuration parsing.
     */
    struct option_t {
      KITTY_DEFAULT_CONSTR_MOVE(option_t)
      /**
       * @brief Copy an encoder option descriptor.
       */
      option_t(const option_t &) = default;

      std::string name;  ///< Encoder command-line option name.
      std::variant<int, int *, std::optional<int> *, std::function<int()>, std::string, std::string *, std::function<const std::string(const config_t &)>> value;  ///< Literal, pointer, or callback that supplies the option value.

      /**
       * @brief Store a named encoder option and its value source.
       *
       * @param name Human-readable name to assign.
       * @param value Literal value, pointer, or callback used to resolve the option.
       */
      option_t(std::string &&name, decltype(value) &&value):
          name {std::move(name)},
          value {std::move(value)} {
      }
    };

    const std::unique_ptr<const encoder_platform_formats_t> platform_formats;  ///< Platform-specific memory and pixel formats accepted by the encoder.

    /**
     * @brief Codec capabilities for AV1, HEVC, or H.264.
     */
    struct codec_t {
      std::vector<option_t> common_options;  ///< Options applied to every encode mode for this codec.
      std::vector<option_t> sdr_options;  ///< Options applied to SDR 4:2:0 streams.
      std::vector<option_t> hdr_options;  ///< Options applied to HDR 4:2:0 streams.
      std::vector<option_t> sdr444_options;  ///< Options applied to SDR 4:4:4 streams.
      std::vector<option_t> hdr444_options;  ///< Options applied to HDR 4:4:4 streams.
      std::vector<option_t> fallback_options;  ///< Options used when the preferred mode cannot be selected.

      std::string name;  ///< Codec name passed to the encoder backend.
      std::bitset<MAX_FLAGS> capabilities;  ///< Capability flags supported by this codec on the encoder.

      /**
       * @brief Test whether a codec capability is enabled.
       *
       * @param flag Feature flag to test or modify.
       * @return True when the requested video flag is set.
       */
      bool operator[](flag_e flag) const {
        return capabilities[(std::size_t) flag];
      }

      /**
       * @brief Return a mutable reference to a codec capability flag.
       *
       * @param flag Feature flag to test or modify.
       * @return Mutable bitset reference for the requested capability.
       */
      std::bitset<MAX_FLAGS>::reference operator[](flag_e flag) {
        return capabilities[(std::size_t) flag];
      }
    };

    codec_t av1;  ///< AV1 codec capability and option set.
    codec_t hevc;  ///< HEVC codec capability and option set.
    codec_t h264;  ///< H.264 codec capability and option set.
    codec_t mpeg2;  ///< MPEG-2 codec capability and option set.
    codec_t h263p;  ///< H.263+ codec capability and option set.

    /**
     * @brief Select the codec descriptor requested by a stream configuration.
     *
     * @param config Configuration values to apply.
     * @return Codec descriptor for H.264, HEVC, AV1, MPEG-2, or H.263+.
     */
    const codec_t &codec_from_config(const config_t &config) const {
      switch (config.videoFormat) {
        default:
          BOOST_LOG(error) << "Unknown video format " << config.videoFormat << ", falling back to H.264";
          // fallthrough
        case format_h264:
          return h264;
        case format_hevc:
          return hevc;
        case format_av1:
          return av1;
        case format_mpeg2:
          return mpeg2;
        case format_h263p:
          return h263p;
      }
    }

    uint32_t flags;  ///< Encoder flags advertised to clients through GameStream capability responses.
  };

  /**
   * @brief Encoder session state shared by capture and encoding threads.
   */
  struct encode_session_t {
    virtual ~encode_session_t() = default;

    /**
     * @brief Convert a captured frame into the encoder's required input representation.
     *
     * @param img Captured image supplied by the platform display backend.
     * @return Zero when the frame was converted or imported successfully.
     */
    virtual int convert(platf::img_t &img) = 0;

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    virtual void request_idr_frame() = 0;

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    virtual void request_normal_frame() = 0;

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    virtual void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;
  };

  // encoders
  extern encoder_t software;

#if !defined(__APPLE__)
  extern encoder_t nvenc;  // available for windows and linux
#endif

#ifdef _WIN32
  extern encoder_t amdvce;
  extern encoder_t quicksync;
  extern encoder_t mediafoundation;
#endif

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  extern encoder_t vaapi;
#endif

#ifdef __APPLE__
  extern encoder_t videotoolbox;
#endif

  /**
   * @brief Encoded packet wrapper used by the streaming pipeline.
   */
  struct packet_raw_t {
    virtual ~packet_raw_t() = default;

    /**
     * @brief Report whether this packet starts an IDR frame.
     *
     * @return True when this frame is an IDR frame.
     */
    virtual bool is_idr() = 0;

    /**
     * @brief Return the frame index associated with this encoded packet.
     *
     * @return Monotonic frame index assigned to this frame.
     */
    virtual int64_t frame_index() = 0;

    /**
     * @brief Return writable access to the encoded packet bytes.
     *
     * @return Pointer to the first byte of encoded frame data.
     */
    virtual uint8_t *data() = 0;

    /**
     * @brief Return the encoded payload length.
     *
     * @return Size of the encoded frame payload in bytes.
     */
    virtual size_t data_size() = 0;

    /**
     * @brief Packet byte-range replacement descriptor.
     */
    struct replace_t {
      std::string_view old;  ///< Byte sequence to find in the encoded packet.
      std::string_view _new;  ///< Byte sequence that replaces `old` before transmission.

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      /**
       * @brief Copy the non-owning byte views for an encoded-byte replacement rule.
       *
       * @param old Byte sequence to replace in encoded packets.
       * @param _new Replacement byte sequence inserted into encoded packets.
       */
      replace_t(std::string_view old, std::string_view _new) noexcept:
          old {old},
          _new {_new} {
      }
    };

    std::vector<replace_t> *replacements = nullptr;  ///< Optional encoded-byte substitutions applied before packetization.
    stream::session_t *channel_data = nullptr;  ///< Stream session that receives this packet.
    bool after_ref_frame_invalidation = false;  ///< Whether the frame follows reference-frame invalidation.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< Capture timestamp associated with the frame.
  };

  /**
   * @brief AVCodec packet wrapper with codec-specific metadata.
   */
  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    /**
     * @brief Report whether the FFmpeg packet is marked as a key frame.
     *
     * @return True when this frame is an IDR frame.
     */
    bool is_idr() override {
      return av_packet->flags & AV_PKT_FLAG_KEY;
    }

    /**
     * @brief Return the FFmpeg packet PTS used as Sunshine's frame index.
     *
     * @return Monotonic frame index assigned to this frame.
     */
    int64_t frame_index() override {
      return av_packet->pts;
    }

    /**
     * @brief Return writable access to the FFmpeg packet payload.
     *
     * @return Pointer to the first encoded byte in the FFmpeg packet.
     */
    uint8_t *data() override {
      return av_packet->data;
    }

    /**
     * @brief Return the FFmpeg packet payload length.
     *
     * @return Size of the encoded frame payload in bytes.
     */
    size_t data_size() override {
      return av_packet->size;
    }

    AVPacket *av_packet;  ///< FFmpeg packet that owns encoded data and frame metadata.
  };

  /**
   * @brief Generic encoded packet bytes and metadata.
   */
  struct packet_raw_generic: packet_raw_t {
    /**
     * @brief Wrap generic encoded frame bytes in Sunshine packet metadata.
     *
     * @param frame_data Encoded frame bytes produced by a non-FFmpeg encoder.
     * @param frame_index Monotonic frame index assigned to the encoded frame.
     * @param idr Whether the packet begins an IDR frame.
     */
    packet_raw_generic(std::vector<uint8_t> &&frame_data, int64_t frame_index, bool idr):
        frame_data {std::move(frame_data)},
        index {frame_index},
        idr {idr} {
    }

    /**
     * @brief Report whether the generic packet starts an IDR frame.
     *
     * @return True when this frame is an IDR frame.
     */
    bool is_idr() override {
      return idr;
    }

    /**
     * @brief Return the stored frame index for this generic packet.
     *
     * @return Monotonic frame index assigned to this frame.
     */
    int64_t frame_index() override {
      return index;
    }

    /**
     * @brief Return writable access to the generic packet payload.
     *
     * @return Pointer to the first encoded byte in `frame_data`.
     */
    uint8_t *data() override {
      return frame_data.data();
    }

    /**
     * @brief Return the generic packet payload length.
     *
     * @return Size of the encoded frame payload in bytes.
     */
    size_t data_size() override {
      return frame_data.size();
    }

    std::vector<uint8_t> frame_data;  ///< Encoded frame bytes owned by this packet.
    int64_t index;  ///< Monotonic frame index assigned to this packet.
    bool idr;  ///< Whether the packet belongs to an IDR frame.
  };

  /**
   * @brief Owning pointer to an encoded packet abstraction.
   */
  using packet_t = std::unique_ptr<packet_raw_t>;

  /**
   * @brief Raw HDR metadata fields parsed from the display backend.
   */
  struct hdr_info_raw_t {
    /**
     * @brief Initialize HDR metadata with only the enabled state.
     *
     * @param enabled Whether the feature should be enabled.
     */
    explicit hdr_info_raw_t(bool enabled):
        enabled {enabled},
        metadata {} {};
    /**
     * @brief Initialize HDR metadata with display metadata from the backend.
     *
     * @param enabled Whether the feature should be enabled.
     * @param metadata Output structure populated with HDR metadata.
     */
    explicit hdr_info_raw_t(bool enabled, const SS_HDR_METADATA &metadata):
        enabled {enabled},
        metadata {metadata} {};

    bool enabled;  ///< Whether HDR mode should be enabled.
    SS_HDR_METADATA metadata;  ///< Display HDR metadata forwarded to the encoder and client.
  };

  /**
   * @brief Owning pointer to optional HDR metadata for a captured display.
   */
  using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;

  extern int active_hevc_mode;  ///< Active HEVC codec advertisement mode.
  extern int active_av1_mode;  ///< Active AV1 codec advertisement mode.
  extern int active_mpeg2_mode;  ///< Active MPEG-2 codec advertisement mode.
  extern int active_h263p_mode;  ///< Active H.263+ codec advertisement mode.
  extern bool last_encoder_probe_supported_ref_frames_invalidation;
  extern std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1
  extern std::array<bool, 3> last_encoder_probe_supported_dynamic_range_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1
  extern std::array<bool, 3> last_encoder_probe_supported_dynamic_range_yuv444_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1

  /**
   * @brief Resolve a client-requested dynamic range against probed encoder capabilities.
   *
   * @param encoder Selected encoder and its probed codec capabilities.
   * @param config Client-requested stream configuration.
   * @return Effective stream configuration, downgraded to SDR when HDR is unsupported.
   */
  config_t resolve_dynamic_range(const encoder_t &encoder, config_t config);

  /**
   * @brief Clear all published encoder probe results.
   */
  void reset_encoder_probe_results();

  /**
   * @brief Publish the capabilities of a probed encoder for client advertisement.
   * @details A codec that did not pass its probe publishes no capabilities.
   *
   * @param encoder Encoder whose probed codec capabilities should be published.
   */
  void record_encoder_probe_results(const encoder_t &encoder);

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
  );

  /**
   * @brief Validate encoder before it is used.
   *
   * @param encoder Encoder configuration or encoder instance.
   * @param expect_failure Expect failure.
   * @return True when encoder validation matches `expect_failure`.
   */
  bool validate_encoder(encoder_t &encoder, bool expect_failure);

  /**
   * @brief Probe encoders and select the preferred encoder.
   * This is called once at startup and each time a stream is launched to
   * ensure the best encoder is selected. Encoder availability can change
   * at runtime due to all sorts of things from driver updates to eGPUs.
   *
   * @warning This is only safe to call when there is no client actively streaming.
   * @return 0 when a usable encoder is selected; nonzero when probing fails.
   */
  int probe_encoders();

  // Several NTSC standard refresh rates are hardcoded here, because their
  // true rate requires a denominator of 1001. ffmpeg's av_d2q() would assume it could
  // reduce 29.97 to 2997/100 but this would be slightly wrong. We also include
  // support for 23.976 film in case someone wants to stream a film at the perfect
  // framerate.
  /**
   * @brief Convert a framerate stored as hundredths of Hz to an FFmpeg rational.
   *
   * @param framerateX100 Framerate multiplied by 100, such as 5994 for 59.94 Hz.
   * @return Rational frame rate preserving NTSC fractional rates when applicable.
   */
  inline AVRational framerateX100_to_rational(const int framerateX100) {
    if (framerateX100 % 2997 == 0) {
      // Multiples of NTSC 29.97 e.g. 59.94, 119.88
      return AVRational {(framerateX100 / 2997) * 30000, 1001};
    }
    switch (framerateX100) {
      case 2397:  // the other weird NTSC framerate, assume these want 23.976 film
      case 2398:
        return AVRational {24000, 1001};
      default:
        // any other fractional rate can be reduced by ffmpeg. Max is set to 1 << 26 based on docs:
        // "rational numbers with |num| <= 1<<26 && |den| <= 1<<26 can be recovered exactly from their double representation"
        return av_d2q((double) framerateX100 / 100.0f, 1 << 26);
    }
  }

  /**
   * @brief Requested framerate as an exact rational.
   * Uses the exact fractional rate when the client provided an X100 value,
   * otherwise the integer framerate over 1.
   *
   * @param config Stream configuration carrying the requested framerate.
   * @return Requested frame rate as a rational.
   */
  inline AVRational framerate_to_rational(const config_t &config) {
    if (config.framerateX100 > 0) {
      return framerateX100_to_rational(config.framerateX100);
    }
    return AVRational {config.framerate, 1};
  }

  /**
   * @brief Scale from logical to physical pixels for the touch port.
   * @details A rotated output (odd transform) is captured in its native orientation, so its logical size is compared
   * with the physical size swapped.
   *
   * @param width Physical width of the captured display.
   * @param height Physical height of the captured display.
   * @param logical_width Logical width of the display, as rotated.
   * @param logical_height Logical height of the display, as rotated.
   * @param transform Wayland output transform of the display.
   * @return Physical pixels per logical pixel.
   */
  inline float touch_port_scale(float width, float height, float logical_width, float logical_height, int transform) {
    if (transform % 2 == 1) {
      std::swap(logical_width, logical_height);
    }
    return std::fminf(width / logical_width, height / logical_height);
  }

  /**
   * @brief Capture frame interval for the requested framerate.
   * Uses the exact fractional rate when the client provided an X100 value.
   *
   * @param config Stream configuration carrying the requested framerate.
   * @return Duration of a single frame interval in nanoseconds.
   */
  inline std::chrono::nanoseconds capture_frame_interval(const config_t &config) {
    const AVRational fps = framerate_to_rational(config);
    return std::chrono::nanoseconds {(static_cast<int64_t>(fps.den) * 1'000'000'000LL) / fps.num};
  }
}  // namespace video
