/**
 * @file src/platform/macos/vt_encoder.mm
 * @brief Definitions for the native VideoToolbox encoder on macOS.
 *
 * Encodes captured CVPixelBuffers directly through a VTCompressionSession. The session is driven strictly one frame in flight: each
 * submission blocks until the output callback delivers the encoded frame, so packets
 * come back in submission order with exactly one packet per frame index.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

// platform includes
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

// local includes
#include "src/logging.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/vt_encoder.h"
#include "src/utility.h"
#include "src/video.h"

namespace {
  using namespace std::literals;

  /**
   * @brief Blocking one-frame-in-flight wrapper around a VTCompressionSession.
   */
  class vt_compression_session_t {
  public:
    ~vt_compression_session_t() {
      if (session) {
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        // Guarantees no output callbacks after return
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
      }
    }

    /**
     * @brief Create and configure the compression session.
     *
     * @param codec CoreMedia codec type to encode.
     * @param config Client stream configuration negotiated for this session.
     * @param colorspace Colorimetry information used for encoding.
     * @return True when the session is ready to encode.
     */
    bool init(CMVideoCodecType codec, const video::config_t &config, const video::sunshine_colorspace_t &colorspace) {
      width = config.width;
      height = config.height;
      framerate = config.framerate;

#if defined(__arm64__) || defined(__aarch64__)
      constexpr bool arm64 = true;
#else
      constexpr bool arm64 = false;
#endif
      const bool try_low_latency = platf::vt::try_low_latency(codec, arm64);
      bool low_latency = try_low_latency && create_session(codec, config, true);
      if (!low_latency && !create_session(codec, config, false)) {
        BOOST_LOG(error) << "VideoToolbox could not create a compression session"sv;
        return false;
      }
      if (try_low_latency && !low_latency) {
        BOOST_LOG(info) << "VideoToolbox low-latency rate control is unavailable; using standard rate control"sv;
      }
      BOOST_LOG(info) << "VideoToolbox session uses "sv << (low_latency ? "low-latency"sv : "standard"sv) << " rate control"sv;

      set_property(kVTCompressionPropertyKey_RealTime, config::video.vt.vt_realtime ? kCFBooleanTrue : kCFBooleanFalse, "RealTime"sv);
      set_property(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, "AllowFrameReordering"sv);
      if (codec == kCMVideoCodecType_H264) {
        // Low-latency H.264 sessions mark pictures as long-term references by default; some
        // client hardware decoders mishandle that and freeze, so keep to short-term references.
        set_property(kVTCompressionPropertyKey_EnableLTR, kCFBooleanFalse, "EnableLTR"sv);
        if (const auto entropy_mode = platf::vt::h264_entropy_mode(config::video.vt.vt_coder)) {
          set_property(kVTCompressionPropertyKey_H264EntropyMode, entropy_mode, "H264EntropyMode"sv);
        }
      }

      CFStringRef profile;
      if (codec == kCMVideoCodecType_HEVC) {
        profile = colorspace.bit_depth == 10 ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
      } else {
        profile = kVTProfileLevel_H264_High_AutoLevel;
      }
      if (!set_property(kVTCompressionPropertyKey_ProfileLevel, profile, "ProfileLevel"sv) && colorspace.bit_depth == 10) {
        // 10-bit output without the Main10 profile would be broken
        return false;
      }

      // Honor the host-side bitrate ceiling, matching the avcodec path
      const int bitrate_kbps = config::video.max_bitrate > 0 ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate;
      // No DataRateLimits under low-latency rate control: with a hard byte window it drops frames
      // that would exceed it (e.g. full-screen changes at 4K120). Without it, AverageBitRate still
      // steers the rate and oversized frames cost quality on later frames instead of a drop.
      set_int_property(kVTCompressionPropertyKey_AverageBitRate, bitrate_kbps * 1000, "AverageBitRate"sv);
      if (!low_latency) {
        // Standard rate control treats AverageBitRate as a long-term target, so bound each second
        // to the bitrate, matching what the avcodec path requested for these sessions.
        set_data_rate_limit(bitrate_kbps * 1000 / 8);
      }
      const int expected_rate = platf::vt::expected_frame_rate(codec, config.width, config.height, config.framerate);
      if (expected_rate != config.framerate) {
        BOOST_LOG(info) << "VideoToolbox H.264 expected frame rate capped at "sv << expected_rate << " (level 5.2 limit for "sv
                        << config.width << 'x' << config.height << ")"sv;
      }
      set_int_property(kVTCompressionPropertyKey_ExpectedFrameRate, expected_rate, "ExpectedFrameRate"sv);
      set_int_property(kVTCompressionPropertyKey_MaxKeyFrameInterval, std::numeric_limits<int>::max(), "MaxKeyFrameInterval"sv);
      apply_colorspace(colorspace);

      const auto prepare_status = VTCompressionSessionPrepareToEncodeFrames(session);
      if (prepare_status != noErr) {
        BOOST_LOG(warning) << "VTCompressionSessionPrepareToEncodeFrames failed: "sv << prepare_status;
      }
      return true;
    }

    /**
     * @brief Encode one frame, blocking until the encoded output is available.
     *
     * @param pixel_buffer Captured pixel buffer to encode.
     * @param frame_index Monotonic frame index assigned by the video pipeline.
     * @param force_idr Whether the frame must be encoded as an IDR frame.
     * @return Encoded frame, or one with empty data on failure.
     */
    platf::vt_encoded_frame encode(CVPixelBufferRef pixel_buffer, uint64_t frame_index, bool force_idr) {
      if (!session) {
        return {};
      }

      if (CVPixelBufferGetWidth(pixel_buffer) != (size_t) width || CVPixelBufferGetHeight(pixel_buffer) != (size_t) height) {
        BOOST_LOG(error) << "VideoToolbox captured frame is "sv << CVPixelBufferGetWidth(pixel_buffer) << 'x' << CVPixelBufferGetHeight(pixel_buffer)
                         << " but the session expects "sv << width << 'x' << height;
        return {};
      }

      {
        std::lock_guard lg {mutex};
        if (need_idr) {
          force_idr = true;
        }
        ready = false;
        failed = false;
        annexb.clear();
      }

      CFDictionaryRef frame_options = nullptr;
      if (force_idr) {
        CFMutableDictionaryRef options = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(options, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        frame_options = options;
      }
      const auto encode_status = VTCompressionSessionEncodeFrame(session, pixel_buffer, CMTimeMake(frame_index, framerate), kCMTimeInvalid, frame_options, nullptr, nullptr);
      if (frame_options) {
        CFRelease(frame_options);
      }
      if (encode_status != noErr) {
        BOOST_LOG(error) << "VTCompressionSessionEncodeFrame failed: "sv << encode_status;
        return {};
      }

      std::unique_lock lock {mutex};
      if (!cv.wait_for(lock, 1s, [this] {
            return ready;
          })) {
        BOOST_LOG(error) << "VideoToolbox encode timed out"sv;
        return {};
      }
      if (failed) {
        return {};
      }
      if (force_idr && !idr_out) {
        BOOST_LOG(error) << "VideoToolbox did not honor the IDR request"sv;
      }
      if (idr_out) {
        need_idr = false;
      }

      frame_size_logger.collect_and_log(annexb.size() / 1000.);

      platf::vt_encoded_frame result;
      result.data = std::move(annexb);
      result.frame_index = frame_index;
      result.idr = idr_out;
      return result;
    }

  private:
    static void output_callback(void *refcon, void *, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample_buffer) {
      auto self = static_cast<vt_compression_session_t *>(refcon);
      std::lock_guard lg {self->mutex};
      self->ready = true;
      self->failed = true;
      do {
        if (status != noErr) {
          BOOST_LOG(error) << "VideoToolbox encode failed: "sv << status;
          break;
        }
        if ((flags & kVTEncodeInfo_FrameDropped) || !sample_buffer) {
          // A dropped frame would leave a gap in the frame index sequence, which the
          // client treats as fatal, so the session is torn down and recreated instead.
          BOOST_LOG(error) << "VideoToolbox dropped a frame"sv;
          break;
        }

        bool sync = true;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
          auto dict = (CFDictionaryRef) CFArrayGetValueAtIndex(attachments, 0);
          auto not_sync = (CFBooleanRef) CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
          sync = !not_sync || !CFBooleanGetValue(not_sync);
        }

        // New parameter sets on a non-IDR frame would never reach the client, which only
        // receives them prepended to IDR frames.
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample_buffer);
        if (!sync && self->last_format && format != self->last_format) {
          BOOST_LOG(warning) << "VideoToolbox format description changed without a keyframe, forcing IDR"sv;
          self->need_idr = true;
        }
        self->last_format = format;

        if (!platf::vt::append_annexb(sample_buffer, sync, self->annexb)) {
          self->annexb.clear();
          break;
        }
        self->idr_out = sync;
        self->failed = false;
      } while (false);
      self->cv.notify_one();
    }

    /**
     * @brief Try to create a hardware compression session.
     *
     * @param codec CoreMedia codec type to encode.
     * @param config Client stream configuration negotiated for this session.
     * @param low_latency Whether to request low-latency rate control.
     * @return True when the session was created.
     */
    bool create_session(CMVideoCodecType codec, const video::config_t &config, bool low_latency) {
      CFMutableDictionaryRef specification = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      // VideoToolbox's software encoder is far slower than Sunshine's own software encoder, so
      // fail without hardware and let encoder selection fall back to that instead.
      CFDictionarySetValue(specification, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
      if (low_latency) {
        CFDictionarySetValue(specification, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanTrue);
      }

      const auto status = VTCompressionSessionCreate(kCFAllocatorDefault, config.width, config.height, codec, specification, nullptr, nullptr, output_callback, this, &session);
      CFRelease(specification);
      if (status != noErr || !session) {
        BOOST_LOG(warning) << "VTCompressionSessionCreate failed (low latency: "sv << low_latency << "): "sv << status;
        session = nullptr;
        return false;
      }
      return true;
    }

    /**
     * @brief Limit the encoded data to a number of bytes per second.
     *
     * @param bytes_per_second Maximum encoded bytes in any one-second window.
     */
    void set_data_rate_limit(std::int64_t bytes_per_second) {
      constexpr std::int64_t one_second = 1;
      CFNumberRef bytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes_per_second);
      CFNumberRef seconds = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &one_second);
      const void *values[] = {bytes, seconds};
      CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, values, 2, &kCFTypeArrayCallBacks);
      set_property(kVTCompressionPropertyKey_DataRateLimits, limits, "DataRateLimits"sv);
      CFRelease(limits);
      CFRelease(seconds);
      CFRelease(bytes);
    }

    bool set_property(CFStringRef key, CFTypeRef value, std::string_view name) {
      const auto status = VTSessionSetProperty(session, key, value);
      if (status != noErr) {
        BOOST_LOG(warning) << "VideoToolbox rejected "sv << name << ": "sv << status;
        return false;
      }
      return true;
    }

    bool set_int_property(CFStringRef key, int value, std::string_view name) {
      CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
      const auto ok = set_property(key, number, name);
      CFRelease(number);
      return ok;
    }

    void apply_colorspace(const video::sunshine_colorspace_t &colorspace) {
      CFStringRef primaries;
      CFStringRef transfer;
      CFStringRef matrix;
      switch (colorspace.colorspace) {
        case video::colorspace_e::rec601:
          primaries = kCVImageBufferColorPrimaries_SMPTE_C;
          transfer = kCVImageBufferTransferFunction_ITU_R_709_2;
          matrix = kCVImageBufferYCbCrMatrix_ITU_R_601_4;
          break;
        case video::colorspace_e::rec709:
          primaries = kCVImageBufferColorPrimaries_ITU_R_709_2;
          transfer = kCVImageBufferTransferFunction_ITU_R_709_2;
          matrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
          break;
        case video::colorspace_e::bt2020sdr:
          primaries = kCVImageBufferColorPrimaries_ITU_R_2020;
          transfer = kCVImageBufferTransferFunction_ITU_R_709_2;
          matrix = kCVImageBufferYCbCrMatrix_ITU_R_2020;
          break;
        case video::colorspace_e::bt2020:
          primaries = kCVImageBufferColorPrimaries_ITU_R_2020;
          transfer = kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
          matrix = kCVImageBufferYCbCrMatrix_ITU_R_2020;
          break;
      }
      set_property(kVTCompressionPropertyKey_ColorPrimaries, primaries, "ColorPrimaries"sv);
      set_property(kVTCompressionPropertyKey_TransferFunction, transfer, "TransferFunction"sv);
      set_property(kVTCompressionPropertyKey_YCbCrMatrix, matrix, "YCbCrMatrix"sv);
    }

    VTCompressionSessionRef session = nullptr;
    int width = 0;
    int height = 0;
    int framerate = 60;

    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    bool failed = false;
    bool idr_out = false;
    bool need_idr = false;
    std::vector<uint8_t> annexb;
    logging::min_max_avg_periodic_logger<double> frame_size_logger = {debug, "VideoToolbox: encoded frame sizes in kB", ""};  ///< Periodic encoded frame size statistics.
    // Pointer identity only, never dereferenced; used to notice mid-stream parameter set changes
    CMFormatDescriptionRef last_format = nullptr;
  };

  /**
   * @brief Native VideoToolbox encode device bound to a macOS capture pipeline.
   */
  class vt_encode_device final: public platf::vt_encode_device_t {
  public:
    /**
     * @brief Bind the device to the capture display and select the capture pixel format.
     *
     * @param pix_fmt Sunshine pixel format to capture and encode.
     * @param resolution_fn Callback that resizes the capture output.
     * @param pixel_format_fn Callback that selects the capture pixel format.
     * @return 0 on success.
     */
    int init(platf::pix_fmt_e pix_fmt, platf::vt::resolution_fn_t resolution_fn, const platf::vt::pixel_format_fn_t &pixel_format_fn) {
      this->resolution_fn = std::move(resolution_fn);
      // Must happen before capture starts; the capture backend snapshots it when the stream is created
      pixel_format_fn(pix_fmt == platf::pix_fmt_e::p010 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
      return 0;
    }

    bool init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) override {
      if (client_config.videoFormat == 2) {
        BOOST_LOG(info) << "VideoToolbox encoder doesn't support AV1"sv;
        return false;
      }
      if (client_config.videoFormat == 0 && colorspace.bit_depth == 10) {
        BOOST_LOG(error) << "VideoToolbox encoder doesn't support 10-bit H.264"sv;
        return false;
      }

      resolution_fn(client_config.width, client_config.height);

      session = std::make_unique<vt_compression_session_t>();
      const CMVideoCodecType codec = client_config.videoFormat == 1 ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
      if (!session->init(codec, client_config, colorspace)) {
        session.reset();
        return false;
      }
      return true;
    }

    int convert(platf::img_t &img) override {
      auto av_img = (platf::av_img_t *) &img;
      if (!av_img->pixel_buffer || !av_img->pixel_buffer->buf) {
        return -1;
      }

      // Keep the frame's backing sample buffer retained while VideoToolbox reads it;
      // the img_t pool recycles the image (and releases its buffers) at any time.
      // Release order matters: the old pixel buffer must unlock (av_pixel_buf_t dtor)
      // before the old sample buffer that owns it is released.
      pixel_buffer = av_img->pixel_buffer;
      sample_buffer = av_img->sample_buffer;
      return 0;
    }

    platf::vt_encoded_frame encode_frame(uint64_t frame_index, bool force_idr) override {
      if (!session || !pixel_buffer) {
        return {};
      }
      return session->encode(pixel_buffer->buf, frame_index, force_idr);
    }

  private:
    platf::vt::resolution_fn_t resolution_fn;
    std::unique_ptr<vt_compression_session_t> session;
    std::shared_ptr<platf::av_sample_buf_t> sample_buffer;
    std::shared_ptr<platf::av_pixel_buf_t> pixel_buffer;
  };
}  // namespace

namespace platf::vt {
  bool append_annexb(CMSampleBufferRef sample_buffer, bool idr, std::vector<uint8_t> &out) {
    constexpr uint8_t start_code[] = {0, 0, 0, 1};
    const auto start_size = out.size();
    auto restore_out = util::fail_guard([&]() {
      out.resize(start_size);
    });

    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample_buffer);
    if (!format) {
      BOOST_LOG(error) << "VideoToolbox sample buffer has no format description"sv;
      return false;
    }
    const bool hevc = CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_HEVC;

    size_t parameter_set_count = 0;
    int nal_length_size = 4;
    {
      const uint8_t *ps = nullptr;
      size_t ps_size = 0;
      const auto status = hevc ?
                            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, &ps, &ps_size, &parameter_set_count, &nal_length_size) :
                            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &ps, &ps_size, &parameter_set_count, &nal_length_size);
      if (status != noErr) {
        BOOST_LOG(error) << "VideoToolbox parameter set query failed: "sv << status;
        return false;
      }
    }

    if (idr) {
      for (size_t i = 0; i < parameter_set_count; ++i) {
        const uint8_t *ps = nullptr;
        size_t ps_size = 0;
        const auto status = hevc ?
                              CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &ps, &ps_size, nullptr, nullptr) :
                              CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &ps, &ps_size, nullptr, nullptr);
        if (status != noErr || !ps_size) {
          BOOST_LOG(error) << "VideoToolbox parameter set "sv << i << " query failed: "sv << status;
          return false;
        }
        if (i == 0) {
          const int nal_type = hevc ? ((ps[0] >> 1) & 0x3f) : (ps[0] & 0x1f);
          const int expected = hevc ? 32 : 7;  // Moonlight requires VPS-first (HEVC) / SPS-first (H.264) IDR frames
          if (nal_type != expected) {
            BOOST_LOG(error) << "VideoToolbox emitted unexpected leading parameter set NAL type "sv << nal_type;
          }
        }
        out.insert(out.end(), std::begin(start_code), std::end(start_code));
        out.insert(out.end(), ps, ps + ps_size);
      }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
    if (!block) {
      BOOST_LOG(error) << "VideoToolbox sample buffer has no data buffer"sv;
      return false;
    }

    char *data = nullptr;
    size_t contiguous_length = 0;
    size_t total_length = 0;
    CMBlockBufferRef contiguous = nullptr;
    auto status = CMBlockBufferGetDataPointer(block, 0, &contiguous_length, &total_length, &data);
    if (status != kCMBlockBufferNoErr || contiguous_length < total_length) {
      if (CMBlockBufferCreateContiguous(kCFAllocatorDefault, block, nullptr, nullptr, 0, 0, 0, &contiguous) != kCMBlockBufferNoErr) {
        BOOST_LOG(error) << "VideoToolbox block buffer could not be made contiguous"sv;
        return false;
      }
      status = CMBlockBufferGetDataPointer(contiguous, 0, &contiguous_length, &total_length, &data);
      if (status != kCMBlockBufferNoErr || contiguous_length < total_length) {
        CFRelease(contiguous);
        BOOST_LOG(error) << "VideoToolbox contiguous block buffer access failed: "sv << status;
        return false;
      }
    }

    bool ok = true;
    size_t offset = 0;
    while (offset + nal_length_size <= total_length) {
      uint64_t nal_length = 0;
      for (int i = 0; i < nal_length_size; ++i) {
        nal_length = (nal_length << 8) | (uint8_t) data[offset + i];
      }
      offset += nal_length_size;
      if (!nal_length || nal_length > total_length - offset) {
        ok = false;
        break;
      }
      out.insert(out.end(), std::begin(start_code), std::end(start_code));
      out.insert(out.end(), data + offset, data + offset + nal_length);
      offset += nal_length;
    }
    if (offset != total_length) {
      ok = false;
    }
    if (!ok) {
      BOOST_LOG(error) << "VideoToolbox produced a malformed AVCC bitstream"sv;
    }
    if (contiguous) {
      CFRelease(contiguous);
    }
    if (ok) {
      restore_out.disable();
    }
    return ok;
  }

  bool try_low_latency(CMVideoCodecType codec, bool arm64) {
    return codec == kCMVideoCodecType_H264 || arm64;
  }

  CFStringRef h264_entropy_mode(int coder) {
    switch (coder) {
      case 1:
        return kVTH264EntropyMode_CABAC;
      case 2:
        return kVTH264EntropyMode_CAVLC;
      default:
        return nullptr;
    }
  }

  int expected_frame_rate(CMVideoCodecType codec, int width, int height, int framerate) {
    if (codec != kCMVideoCodecType_H264 || width <= 0 || height <= 0) {
      return framerate;
    }

    // H.264 level 5.2 MaxMBPS (macroblocks per second), Table A-1
    constexpr std::int64_t max_macroblocks_per_second = 2073600;
    const std::int64_t macroblocks_per_frame = static_cast<std::int64_t>((width + 15) / 16) * ((height + 15) / 16);
    const auto level_limit = static_cast<int>(std::max<std::int64_t>(1, max_macroblocks_per_second / macroblocks_per_frame));
    return std::min(framerate, level_limit);
  }

  std::unique_ptr<vt_encode_device_t> make_encode_device(pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn) {
    if (pix_fmt != pix_fmt_e::nv12 && pix_fmt != pix_fmt_e::p010) {
      BOOST_LOG(error) << "Unsupported pixel format for the VideoToolbox encoder"sv;
      return nullptr;
    }

    auto device = std::make_unique<::vt_encode_device>();
    if (device->init(pix_fmt, std::move(resolution_fn), pixel_format_fn)) {
      return nullptr;
    }
    return device;
  }
}  // namespace platf::vt
