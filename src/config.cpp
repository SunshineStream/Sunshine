/**
 * @file src/config.cpp
 * @brief Definitions for the configuration of Sunshine.
 */
// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "config.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef _WIN32
  #include <shellapi.h>
#endif

#if defined(_WIN32) && !defined(DOXYGEN)
  #ifdef _GLIBCXX_USE_C99_INTTYPES
    #undef _GLIBCXX_USE_C99_INTTYPES
  #endif
  #include <AMF/components/VideoEncoderAV1.h>
  #include <AMF/components/VideoEncoderHEVC.h>
  #include <AMF/components/VideoEncoderVCE.h>
#endif

#if !defined(__ANDROID__) && !defined(__APPLE__)
  // For NVENC legacy constants
  #include <ffnvcodec/nvEncodeAPI.h>
#endif

#if defined(SUNSHINE_BUILD_VAAPI) && !defined(DOXYGEN)
  // For VAAPI rate control types
  #include <va/va.h>
#endif

namespace fs = std::filesystem;
using namespace std::literals;

constexpr auto CA_DIR = "credentials";  ///< Subdirectory under app data that stores Sunshine credentials.
const std::string PRIVATE_KEY_FILE = std::string(CA_DIR) + "/cakey.pem";  ///< Relative path to the persisted private key PEM file.
const std::string CERTIFICATE_FILE = std::string(CA_DIR) + "/cacert.pem";  ///< Relative path to the persisted certificate PEM file.
const std::string APPS_JSON_PATH = platf::appdata().string() + "/apps.json";  ///< Default path to the applications JSON file.

namespace config {

  namespace nv {

    std::string ffmpeg_preset_from_quality(const int quality_preset) {
      return std::format("p{}", quality_preset);
    }

    /**
     * @brief Parse the `nvenc_twopass` configuration value.
     *
     * @param preset Encoder preset value supplied by the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    nvenc::nvenc_two_pass twopass_from_view(const std::string_view &preset) {
      using enum nvenc::nvenc_two_pass;
      if (preset == "disabled") {
        return disabled;
      }
      if (preset == "quarter_res") {
        return quarter_resolution;
      }
      if (preset == "full_res") {
        return full_resolution;
      }
      BOOST_LOG(warning) << "config: unknown nvenc_twopass value: " << preset;
      return quarter_resolution;
    }

    /**
     * @brief Parse the `nvenc_split_encode` configuration value.
     *
     * @param preset Encoder preset value supplied by the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    nvenc::nvenc_split_frame_encoding split_encode_from_view(const std::string_view &preset) {
      using enum nvenc::nvenc_split_frame_encoding;
      if (preset == "disabled") {
        return disabled;
      }
      if (preset == "driver_decides") {
        return driver_decides;
      }
      if (preset == "enabled") {
        return force_enabled;
      }
      BOOST_LOG(warning) << "config: unknown nvenc_split_encode value: " << preset;
      return driver_decides;
    }

  }  // namespace nv

  namespace amd {
#if !defined(_WIN32) || defined(DOXYGEN)
    // values accurate as of 27/12/2022, but aren't strictly necessary for MacOS build
    constexpr int AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED = 100;  ///< Fallback AMF enum value for av1 quality preset speed.
    constexpr int AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY = 30;  ///< Fallback AMF enum value for av1 quality preset quality.
    constexpr int AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED = 70;  ///< Fallback AMF enum value for av1 quality preset balanced.
    constexpr int AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED = 10;  ///< Fallback AMF enum value for hevc quality preset speed.
    constexpr int AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY = 0;  ///< Fallback AMF enum value for hevc quality preset quality.
    constexpr int AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED = 5;  ///< Fallback AMF enum value for hevc quality preset balanced.
    constexpr int AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED = 1;  ///< Fallback AMF enum value for quality preset speed.
    constexpr int AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY = 2;  ///< Fallback AMF enum value for quality preset quality.
    constexpr int AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED = 0;  ///< Fallback AMF enum value for quality preset balanced.
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP = 0;  ///< Fallback AMF enum value for av1 rate control method constant qp.
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR = 3;  ///< Fallback AMF enum value for av1 rate control method cbr.
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR = 2;  ///< Fallback AMF enum value for av1 rate control method peak constrained vbr.
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR = 1;  ///< Fallback AMF enum value for av1 rate control method latency constrained vbr.
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP = 0;  ///< Fallback AMF enum value for hevc rate control method constant qp.
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR = 3;  ///< Fallback AMF enum value for hevc rate control method cbr.
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR = 2;  ///< Fallback AMF enum value for hevc rate control method peak constrained vbr.
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR = 1;  ///< Fallback AMF enum value for hevc rate control method latency constrained vbr.
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP = 0;  ///< Fallback AMF enum value for rate control method constant qp.
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR = 1;  ///< Fallback AMF enum value for rate control method cbr.
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR = 2;  ///< Fallback AMF enum value for rate control method peak constrained vbr.
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR = 3;  ///< Fallback AMF enum value for rate control method latency constrained vbr.
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING = 0;  ///< Fallback AMF enum value for av1 usage transcoding.
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY = 1;  ///< Fallback AMF enum value for av1 usage low latency.
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY = 2;  ///< Fallback AMF enum value for av1 usage ultra low latency.
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM = 3;  ///< Fallback AMF enum value for av1 usage webcam.
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY = 5;  ///< Fallback AMF enum value for av1 usage low latency high quality.
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING = 0;  ///< Fallback AMF enum value for hevc usage transcoding.
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY = 1;  ///< Fallback AMF enum value for hevc usage ultra low latency.
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY = 2;  ///< Fallback AMF enum value for hevc usage low latency.
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM = 3;  ///< Fallback AMF enum value for hevc usage webcam.
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY = 5;  ///< Fallback AMF enum value for hevc usage low latency high quality.
    constexpr int AMF_VIDEO_ENCODER_USAGE_TRANSCODING = 0;  ///< Fallback AMF enum value for usage transcoding.
    constexpr int AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY = 1;  ///< Fallback AMF enum value for usage ultra low latency.
    constexpr int AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY = 2;  ///< Fallback AMF enum value for usage low latency.
    constexpr int AMF_VIDEO_ENCODER_USAGE_WEBCAM = 3;  ///< Fallback AMF enum value for usage webcam.
    constexpr int AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY = 5;  ///< Fallback AMF enum value for usage low latency high quality.
    constexpr int AMF_VIDEO_ENCODER_UNDEFINED = 0;  ///< Fallback AMF enum value for undefined.
    constexpr int AMF_VIDEO_ENCODER_CABAC = 1;  ///< Fallback AMF enum value for cabac.
    constexpr int AMF_VIDEO_ENCODER_CALV = 2;  ///< Fallback AMF enum value for calv.
#endif

    /**
     * @brief Enumerates supported quality AV1 options.
     */
    enum class quality_av1_e : int {
      speed = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    /**
     * @brief Enumerates supported quality HEVC options.
     */
    enum class quality_hevc_e : int {
      speed = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    /**
     * @brief Enumerates supported quality h264 options.
     */
    enum class quality_h264_e : int {
      speed = AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    /**
     * @brief Enumerates supported rc AV1 options.
     */
    enum class rc_av1_e : int {
      cbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    /**
     * @brief Enumerates supported rc HEVC options.
     */
    enum class rc_hevc_e : int {
      cbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    /**
     * @brief Enumerates supported rc h264 options.
     */
    enum class rc_h264_e : int {
      cbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    /**
     * @brief Enumerates supported usage AV1 options.
     */
    enum class usage_av1_e : int {
      transcoding = AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    /**
     * @brief Enumerates supported usage HEVC options.
     */
    enum class usage_hevc_e : int {
      transcoding = AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    /**
     * @brief Enumerates supported usage h264 options.
     */
    enum class usage_h264_e : int {
      transcoding = AMF_VIDEO_ENCODER_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    /**
     * @brief Enumerates supported coder options.
     */
    enum coder_e : int {
      _auto = AMF_VIDEO_ENCODER_UNDEFINED,  ///< Auto
      cabac = AMF_VIDEO_ENCODER_CABAC,  ///< CABAC
      cavlc = AMF_VIDEO_ENCODER_CALV  ///< CAVLC
    };

    /**
     * @brief Parse an AMD quality preset while preserving the current value on invalid input.
     *
     * @param quality_type Configuration text naming the AMD quality preset.
     * @param original Original text value used when reporting a parsing failure.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    template<class T>
    ::std::optional<int> quality_from_view(const ::std::string_view &quality_type, const ::std::optional<int>(&original)) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (quality_type == #x##sv) \
    return (int) T::x
#endif
      _CONVERT_(balanced);
      _CONVERT_(quality);
      _CONVERT_(speed);
#ifndef DOXYGEN
  #undef _CONVERT_
#endif
      return original;
    }

    /**
     * @brief Parse an AMD rate-control mode while preserving the current value on invalid input.
     *
     * @param rc Rate-control mode selected in the configuration.
     * @param original Original text value used when reporting a parsing failure.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    template<class T>
    ::std::optional<int> rc_from_view(const ::std::string_view &rc, const ::std::optional<int>(&original)) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (rc == #x##sv) \
    return (int) T::x
#endif
      _CONVERT_(cbr);
      _CONVERT_(cqp);
      _CONVERT_(vbr_latency);
      _CONVERT_(vbr_peak);
#ifndef DOXYGEN
  #undef _CONVERT_
#endif
      return original;
    }

    /**
     * @brief Parse an AMD encoder usage mode while preserving the current value on invalid input.
     *
     * @param usage Encoder usage mode selected in the configuration.
     * @param original Original text value used when reporting a parsing failure.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    template<class T>
    ::std::optional<int> usage_from_view(const ::std::string_view &usage, const ::std::optional<int>(&original)) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (usage == #x##sv) \
    return (int) T::x
#endif
      _CONVERT_(lowlatency);
      _CONVERT_(lowlatency_high_quality);
      _CONVERT_(transcoding);
      _CONVERT_(ultralowlatency);
      _CONVERT_(webcam);
#ifndef DOXYGEN
  #undef _CONVERT_
#endif
      return original;
    }

    /**
     * @brief Parse an entropy-coder mode from configuration text.
     *
     * @param coder Entropy-coder mode selected in the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    int coder_from_view(const ::std::string_view &coder) {
      if (coder == "auto"sv) {
        return _auto;
      }
      if (coder == "cabac"sv || coder == "ac"sv) {
        return cabac;
      }
      if (coder == "cavlc"sv || coder == "vlc"sv) {
        return cavlc;
      }

      return _auto;
    }
  }  // namespace amd

  namespace qsv {
    /**
     * @brief Enumerates supported preset options.
     */
    enum preset_e : int {
      veryslow = 1,  ///< veryslow preset
      slower = 2,  ///< slower preset
      slow = 3,  ///< slow preset
      medium = 4,  ///< medium preset
      fast = 5,  ///< fast preset
      faster = 6,  ///< faster preset
      veryfast = 7  ///< veryfast preset
    };

    /**
     * @brief Enumerates supported cavlc options.
     */
    enum cavlc_e : int {
      _auto = false,  ///< Auto
      enabled = true,  ///< Enabled
      disabled = false  ///< Disabled
    };

    /**
     * @brief Parse a QSV encoder preset from configuration text.
     *
     * @param preset Encoder preset value supplied by the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    std::optional<int> preset_from_view(const std::string_view &preset) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (preset == #x##sv) \
    return x
#endif
      _CONVERT_(veryslow);
      _CONVERT_(slower);
      _CONVERT_(slow);
      _CONVERT_(medium);
      _CONVERT_(fast);
      _CONVERT_(faster);
      _CONVERT_(veryfast);
#ifndef DOXYGEN
  #undef _CONVERT_
#endif
      return std::nullopt;
    }

    /**
     * @brief Parse an entropy-coder mode from configuration text.
     *
     * @param coder Entropy-coder mode selected in the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    std::optional<int> coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) {
        return _auto;
      }
      if (coder == "cabac"sv || coder == "ac"sv) {
        return disabled;
      }
      if (coder == "cavlc"sv || coder == "vlc"sv) {
        return enabled;
      }
      return std::nullopt;
    }

  }  // namespace qsv

  namespace vaapi {
#if !defined(SUNSHINE_BUILD_VAAPI) || defined(DOXYGEN)
    constexpr int VA_RC_CBR = 0x00000002;  ///< CBR rate control
    constexpr int VA_RC_VBR = 0x00000004;  ///< VBR rate control
    constexpr int VA_RC_CQP = 0x00000010;  ///< CQP rate control
    constexpr int VA_RC_ICQ = 0x00000040;  ///< ICQ rate control
    constexpr int VA_RC_QVBR = 0x00000400;  ///< QVBR rate control
    constexpr int VA_RC_AVBR = 0x00000800;  ///< AVBR rate control
#endif
    /**
     * @brief Enumerates supported VA-API quality options.
     */
    enum class quality_e : int {
      _auto = 0,  ///< Auto quality level
      speed = 1,  ///< Speed level
      balanced = 2,  ///< Balanced level
      quality = 3  ///< Quality level
    };

    /**
     * @brief Enumerates supported VA-API rc options.
     */
    enum class rc_e : int {
      _auto = 0,  ///< Auto rate control
      avbr = VA_RC_AVBR,  ///< AVBR - average variable bitrate
      cbr = VA_RC_CBR,  ///< CBR - constant bitrate
      cqp = VA_RC_CQP,  ///< CQP - constant QP
      icq = VA_RC_ICQ,  ///< ICQ - intelligent QP
      qvbr = VA_RC_QVBR,  ///< QVBR - quality-defined variable bitrate
      vbr = VA_RC_VBR  ///< VBR - variable bitrate
    };

    /**
     * @brief Parse a VA-API quality preset while preserving the current value on invalid input.
     *
     * @param quality_type Configuration text naming the VA-API quality preset.
     * @param original Original text value used when reporting a parsing failure.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    template<class T>
    ::std::optional<int> quality_from_view(const ::std::string_view &quality_type, const ::std::optional<int>(&original)) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (quality_type == #x##sv) \
    return (int) T::x
#endif
      _CONVERT_(balanced);
      _CONVERT_(quality);
      _CONVERT_(speed);
#ifdef _CONVERT_
  #undef _CONVERT_
#endif
      return original;
    }

    /**
     * @brief Parse a VA-API rate-control mode while preserving the current value on invalid input.
     *
     * @param rc Rate-control mode selected in the configuration.
     * @param original Original text value used when reporting a parsing failure.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    template<class T>
    ::std::optional<int> rc_from_view(const ::std::string_view &rc, const ::std::optional<int>(&original)) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (rc == #x##sv) \
    return (int) T::x
#endif
      _CONVERT_(avbr);
      _CONVERT_(cbr);
      _CONVERT_(cqp);
      _CONVERT_(icq);
      _CONVERT_(qvbr);
      _CONVERT_(vbr);
#ifdef _CONVERT_
  #undef _CONVERT_
#endif
      return original;
    }

  }  // namespace vaapi

  namespace vt {

    /**
     * @brief Enumerates supported coder options.
     */
    enum coder_e : int {
      _auto = 0,  ///< Auto
      cabac,  ///< CABAC
      cavlc  ///< CAVLC
    };

    /**
     * @brief Parse an entropy-coder mode from configuration text.
     *
     * @param coder Entropy-coder mode selected in the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    int coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) {
        return _auto;
      }
      if (coder == "cabac"sv || coder == "ac"sv) {
        return cabac;
      }
      if (coder == "cavlc"sv || coder == "vlc"sv) {
        return cavlc;
      }

      return -1;
    }

    /**
     * @brief Parse the VideoToolbox realtime encoder flag.
     *
     * @param rt Real-time encoder usage selector.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    int rt_from_view(const std::string_view &rt) {
      if (rt == "disabled" || rt == "off" || rt == "0") {
        return 0;
      }

      return 1;
    }

  }  // namespace vt

  namespace sw {
    /**
     * @brief Parse an SVT-AV1 speed preset from configuration text.
     *
     * @param preset Encoder preset value supplied by the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    int svtav1_preset_from_view(const std::string_view &preset) {
#ifndef DOXYGEN
  #define _CONVERT_(x, y) \
    if (preset == #x##sv) \
    return y
#endif
      _CONVERT_(veryslow, 1);
      _CONVERT_(slower, 2);
      _CONVERT_(slow, 4);
      _CONVERT_(medium, 5);
      _CONVERT_(fast, 7);
      _CONVERT_(faster, 9);
      _CONVERT_(veryfast, 10);
      _CONVERT_(superfast, 11);
      _CONVERT_(ultrafast, 12);
#ifndef DOXYGEN
  #undef _CONVERT_
#endif
      return 11;  // Default to superfast
    }
  }  // namespace sw

  namespace dd {
    /**
     * @brief Parse display-device preparation mode from configuration text.
     *
     * @param value Configuration text from the display-device preparation setting.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    video_t::dd_t::config_option_e config_option_from_view(const std::string_view value) {
#ifndef DOXYGEN
  #define _CONVERT_(x) \
    if (value == #x##sv) \
    return video_t::dd_t::config_option_e::x
#endif
      _CONVERT_(disabled);
      _CONVERT_(verify_only);
      _CONVERT_(ensure_active);
      _CONVERT_(ensure_primary);
      _CONVERT_(ensure_only_display);
#ifndef DOXYGEN
  #undef _CONVERT_
#endif
      return video_t::dd_t::config_option_e::disabled;  // Default to this if value is invalid
    }

    /**
     * @brief Parse display-device resolution mode from configuration text.
     *
     * @param value Configuration text from the display-device resolution setting.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    video_t::dd_t::resolution_option_e resolution_option_from_view(const std::string_view value) {
#ifndef DOXYGEN
  #define _CONVERT_2_ARG_(str, val) \
    if (value == #str##sv) \
    return video_t::dd_t::resolution_option_e::val
  #define _CONVERT_(x) _CONVERT_2_ARG_(x, x)
#endif
      _CONVERT_(disabled);
      _CONVERT_2_ARG_(auto, automatic);
      _CONVERT_(manual);
#ifndef DOXYGEN
  #undef _CONVERT_
  #undef _CONVERT_2_ARG_
#endif
      return video_t::dd_t::resolution_option_e::disabled;  // Default to this if value is invalid
    }

    /**
     * @brief Parse display-device refresh-rate mode from configuration text.
     *
     * @param value Configuration text from the display-device refresh-rate setting.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    video_t::dd_t::refresh_rate_option_e refresh_rate_option_from_view(const std::string_view value) {
#ifndef DOXYGEN
  #define _CONVERT_2_ARG_(str, val) \
    if (value == #str##sv) \
    return video_t::dd_t::refresh_rate_option_e::val
  #define _CONVERT_(x) _CONVERT_2_ARG_(x, x)
#endif
      _CONVERT_(disabled);
      _CONVERT_2_ARG_(auto, automatic);
      _CONVERT_(manual);
#ifndef DOXYGEN
  #undef _CONVERT_
  #undef _CONVERT_2_ARG_
#endif
      return video_t::dd_t::refresh_rate_option_e::disabled;  // Default to this if value is invalid
    }

    /**
     * @brief Parse display-device HDR mode from configuration text.
     *
     * @param value Configuration text from the display-device HDR setting.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    video_t::dd_t::hdr_option_e hdr_option_from_view(const std::string_view value) {
#ifndef DOXYGEN
  #define _CONVERT_2_ARG_(str, val) \
    if (value == #str##sv) \
    return video_t::dd_t::hdr_option_e::val
  #define _CONVERT_(x) _CONVERT_2_ARG_(x, x)
#endif
      _CONVERT_(disabled);
      _CONVERT_2_ARG_(auto, automatic);
#ifndef DOXYGEN
  #undef _CONVERT_
  #undef _CONVERT_2_ARG_
#endif
      return video_t::dd_t::hdr_option_e::disabled;  // Default to this if value is invalid
    }

    /**
     * @brief Parse display-mode remapping rules from JSON configuration text.
     *
     * @param value JSON array text from the display-device mode-remapping setting.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    video_t::dd_t::mode_remapping_t mode_remapping_from_view(const std::string_view value) {
      const auto parse_entry_list {[](const auto &entry_list, auto &output_field) {
        for (auto &[_, entry] : entry_list) {
          auto requested_resolution = entry.template get_optional<std::string>("requested_resolution"s);
          auto requested_fps = entry.template get_optional<std::string>("requested_fps"s);
          auto final_resolution = entry.template get_optional<std::string>("final_resolution"s);
          auto final_refresh_rate = entry.template get_optional<std::string>("final_refresh_rate"s);

          output_field.push_back(video_t::dd_t::mode_remapping_entry_t {requested_resolution.value_or(""), requested_fps.value_or(""), final_resolution.value_or(""), final_refresh_rate.value_or("")});
        }
      }};

      // We need to add a wrapping object to make it valid JSON, otherwise ptree cannot parse it.
      std::stringstream json_stream;
      json_stream << "{\"dd_mode_remapping\":" << value << "}";

      boost::property_tree::ptree json_tree;
      boost::property_tree::read_json(json_stream, json_tree);

      video_t::dd_t::mode_remapping_t output;
      parse_entry_list(json_tree.get_child("dd_mode_remapping.mixed"), output.mixed);
      parse_entry_list(json_tree.get_child("dd_mode_remapping.resolution_only"), output.resolution_only);
      parse_entry_list(json_tree.get_child("dd_mode_remapping.refresh_rate_only"), output.refresh_rate_only);

      return output;
    }
  }  // namespace dd

  /**
   * @brief Default video configuration values used before file and CLI overrides.
   */
  video_t video {
    28,  // qp

    0,  // hevc_mode
    0,  // av1_mode

    2,  // min_threads

    0,  // max_stream_width
    0,  // max_stream_height
    {
      "superfast"s,  // preset
      "zerolatency"s,  // tune
      11,  // superfast
    },  // software

    {},  // nv
    true,  // nv_realtime_hags
    true,  // nv_opengl_vulkan_on_dxgi
    true,  // nv_sunshine_high_power_mode
    {},  // nv_legacy

    {
      qsv::medium,  // preset
      qsv::_auto,  // cavlc
      false,  // slow_hevc
    },  // qsv

    {
      std::to_underlying(amd::usage_h264_e::ultralowlatency),  // usage (h264)
      std::to_underlying(amd::usage_hevc_e::ultralowlatency),  // usage (hevc)
      std::to_underlying(amd::usage_av1_e::ultralowlatency),  // usage (av1)
      std::to_underlying(amd::rc_h264_e::vbr_latency),  // rate control (h264)
      std::to_underlying(amd::rc_hevc_e::vbr_latency),  // rate control (hevc)
      std::to_underlying(amd::rc_av1_e::vbr_latency),  // rate control (av1)
      0,  // enforce_hrd
      std::to_underlying(amd::quality_h264_e::balanced),  // quality (h264)
      std::to_underlying(amd::quality_hevc_e::balanced),  // quality (hevc)
      std::to_underlying(amd::quality_av1_e::balanced),  // quality (av1)
      0,  // preanalysis
      1,  // vbaq
      {},  // max_au_size (disabled by default)
      std::to_underlying(amd::coder_e::_auto),  // coder
    },  // amd

    {
      1,
      -1,
    },  // vt

    {
      0,  // blbrc
      std::to_underlying(vaapi::quality_e::_auto),  // quality
      std::to_underlying(vaapi::rc_e::_auto),  // rate control
      {},  // rate control string
      false,  // strict_rc_buffer
    },  // vaapi

    {
      2,  // vk.tune (default: ll - low latency)
      2,  // vk.rc_mode (default: cbr)
      2,  // vk.quality (default: 2 = balanced, 1 = speed, 3 = quality)
    },

    {},  // capture
    false,  // fp16_sdr_gamma_encoded
    {},  // encoder
    {},  // adapter_name
    {},  // output_name

    {
      video_t::dd_t::config_option_e::disabled,  // configuration_option
      video_t::dd_t::resolution_option_e::automatic,  // resolution_option
      {},  // manual_resolution
      video_t::dd_t::refresh_rate_option_e::automatic,  // refresh_rate_option
      {},  // manual_refresh_rate
      video_t::dd_t::hdr_option_e::automatic,  // hdr_option
      3s,  // config_revert_delay
      {},  // config_revert_on_disconnect
      {},  // mode_remapping
      {}  // wa
    },  // display_device

    0,  // max_bitrate
    0  // minimum_fps_target (0 = framerate)
  };

  /**
   * @brief Default audio configuration values used before file and CLI overrides.
   */
  audio_t audio {
    {},  // audio_sink
    {},  // virtual_sink
    true,  // stream audio
    true,  // install_steam_drivers
    false,  // external_audio
  };

  /**
   * @brief Default stream configuration values used before file and CLI overrides.
   */
  stream_t stream {
    10s,  // ping_timeout

    APPS_JSON_PATH,

    20,  // fecPercentage

    ENCRYPTION_MODE_NEVER,  // lan_encryption_mode
    ENCRYPTION_MODE_OPPORTUNISTIC,  // wan_encryption_mode
    0,  // packetsize
    true,  // dscp_tagging
  };

  /**
   * @brief Default NVHTTP server configuration values used before file and CLI overrides.
   */
  nvhttp_t nvhttp {
    "lan",  // origin web manager

    PRIVATE_KEY_FILE,
    CERTIFICATE_FILE,

    platf::get_host_name(),  // sunshine_name,
    "sunshine_state.json"s,  // file_state
    {},  // external_ip
  };

  /**
   * @brief Default input configuration values used before file and CLI overrides.
   */
  input_t input {
    {
      {0x10, 0xA0},
      {0x11, 0xA2},
      {0x12, 0xA4},
    },
    -1ms,  // back_button_timeout
    500ms,  // key_repeat_delay
    std::chrono::duration<double> {1 / 24.9},  // key_repeat_period

    "auto",  // Default gamepad profile.
    {},  // Windows requests a backend choice; macOS defaults to Virtual HID Broker.
    true,  // back as touchpad click enabled for PlayStation-style gamepads
    true,  // client gamepads with motion events use PlayStation-style emulation
    true,  // client gamepads with touchpads use PlayStation-style emulation
    true,  // virtualhid_randomize_mac

    true,  // keyboard enabled
    false,  // key_rightalt_to_key_win
    true,  // mouse enabled
    true,  // controller enabled
    true,  // always send scancodes
    true,  // high resolution scrolling
    true,  // native pen/touch support
  };

  /**
   * @brief Default top-level Sunshine configuration values used before file and CLI overrides.
   */
  sunshine_t sunshine {
    "en",  // locale
    2,  // min_log_level
    0,  // flags
    {},  // User file
    {},  // Username
    {},  // Password
    {},  // Password Salt
    platf::appdata().string() + "/sunshine.conf",  // config file
    {},  // cmd args
    47989,  // Base port number
    "ipv4",  // Address family
    {},  // Bind address
    platf::appdata().string() + "/sunshine.log",  // log file
    false,  // notify_pre_releases
    true,  // system_tray
    {},  // prep commands
    {},  // csrf_allowed_origins
  };

  /**
   * @brief Return whether a character terminates a configuration line.
   *
   * @param ch Character currently being classified by the parser.
   * @return True when the tested parser condition is met.
   */
  bool endline(char ch) {
    return ch == '\r' || ch == '\n';
  }

  /**
   * @brief Return whether a character is horizontal parser whitespace.
   *
   * @param ch Character currently being classified by the parser.
   * @return True when the tested parser condition is met.
   */
  bool space_tab(char ch) {
    return ch == ' ' || ch == '\t';
  }

  /**
   * @brief Return whether a character should be treated as parser whitespace.
   *
   * @param ch Character currently being classified by the parser.
   * @return True when the tested parser condition is met.
   */
  bool whitespace(char ch) {
    return space_tab(ch) || endline(ch);
  }

  /**
   * @brief Copy a configuration text range while stripping inline comments.
   *
   * @param begin Iterator or pointer marking the start of the input range.
   * @param end Iterator or pointer marking the end of the input range.
   * @return Value converted to string.
   */
  std::string to_string(const char *begin, const char *end) {
    std::string result;

    KITTY_WHILE_LOOP(auto pos = begin, pos != end, {
      auto comment = std::find(pos, end, '#');
      auto endl = std::find_if(comment, end, endline);

      result.append(pos, comment);

      pos = endl;
    })

    return result;
  }

  /**
   * @brief Advance over a bracketed list while honoring nested brackets.
   *
   * @param skipper Function used to skip characters while parsing.
   * @param end Iterator or pointer marking the end of the input range.
   * @return Iterator positioned after the matching closing bracket or at the end.
   */
  template<class It>
  It skip_list(It skipper, It end) {
    int stack = 1;
    while (skipper != end && stack) {
      if (*skipper == '[') {
        ++stack;
      }
      if (*skipper == ']') {
        --stack;
      }

      ++skipper;
    }

    return skipper;
  }

  std::pair<
    std::string_view::const_iterator,
    std::optional<std::pair<std::string, std::string>>>
    /**
     * @brief Parse one `name = value` configuration entry.
     *
     * @param begin Iterator or pointer marking the start of the input range.
     * @param end Iterator or pointer marking the end of the input range.
     * @return Iterator for the next line and the parsed key-value pair when one was found.
     */
    parse_option(std::string_view::const_iterator begin, std::string_view::const_iterator end) {
    begin = std::find_if_not(begin, end, whitespace);
    auto endl = std::find_if(begin, end, endline);
    auto endc = std::find(begin, endl, '#');
    endc = std::find_if(std::make_reverse_iterator(endc), std::make_reverse_iterator(begin), std::not_fn(whitespace)).base();

    auto eq = std::find(begin, endc, '=');
    if (eq == endc || eq == begin) {
      return std::make_pair(endl, std::nullopt);
    }

    auto end_name = std::find_if_not(std::make_reverse_iterator(eq), std::make_reverse_iterator(begin), space_tab).base();
    auto begin_val = std::find_if_not(eq + 1, endc, space_tab);

    if (begin_val == endl) {
      return std::make_pair(endl, std::nullopt);
    }

    // Lists might contain newlines
    if (*begin_val == '[') {
      endl = skip_list(begin_val + 1, end);

      // Check if we reached the end of the file without finding a closing bracket
      // We know we have a valid closing bracket if:
      // 1. We didn't reach the end, or
      // 2. We reached the end but the last character was the matching closing bracket
      if (endl == end && end == begin_val + 1) {
        BOOST_LOG(warning) << "config: Missing ']' in config option: " << to_string(begin, end_name);
        return std::make_pair(endl, std::nullopt);
      }
    }

    return std::make_pair(
      endl,
      std::make_pair(to_string(begin, end_name), to_string(begin_val, endl))
    );
  }

  /**
   * @brief Parse Sunshine configuration text into key-value entries.
   */
  config_map_t parse_config(const std::string_view &file_content) {
    config_map_t vars;

    auto pos = std::begin(file_content);
    auto end = std::end(file_content);

    while (pos < end) {
      TUPLE_2D(endl, var, parse_option(pos, end));

      pos = endl;
      if (pos != end) {
        pos += (*pos == '\r') ? 2 : 1;
      }

      if (!var) {
        continue;
      }

      vars.try_emplace(std::move(var->first), std::move(var->second));
    }

    return vars;
  }

  bool persist_config_option_if_missing(const std::string_view name, const std::string_view value) {
    auto file_content = file_handler::read_file(sunshine.config_file.c_str());
    if (parse_config(file_content).contains(std::string {name})) {
      return false;
    }

    if (!file_content.empty() && file_content.back() != '\n') {
      file_content += '\n';
    }
    file_content += std::format("{} = {}\n", name, value);
    if (file_handler::write_file(sunshine.config_file.c_str(), file_content) != 0) {
      BOOST_LOG(warning) << "Failed to persist automatically selected config option '"sv << name << "'"sv;
      return false;
    }

    BOOST_LOG(info) << "Automatically selected config option '"sv << name << "' = "sv << value;
    return true;
  }

  bool select_all_gamepad_drivers_if_licensed(const bool virtualhid_licensed) {
    if (!virtualhid_licensed || !input.gamepad_driver.empty() || !persist_config_option_if_missing("gamepad_driver", GAMEPAD_DRIVER_ALL)) {
      return false;
    }

    input.gamepad_driver = GAMEPAD_DRIVER_ALL;
    return true;
  }

  /**
   * @brief Consume a string setting from the parsed configuration map.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void string_f(config_map_t &vars, const std::string &name, std::string &destination) {
    auto it = vars.find(name);
    if (it == std::end(vars)) {
      return;
    }

    destination = std::move(it->second);

    vars.erase(it);
  }

  /**
   * @brief Consume a setting and convert it with a caller-provided parser.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   * @param f Converter applied to the raw configuration string.
   */
  template<typename T, typename F>
  void generic_f(config_map_t &vars, const std::string &name, T &destination, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      destination = f(tmp);
    }
  }

  /**
   * @brief Consume a string setting only when it matches an allowed value.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   * @param allowed_vals Accepted string values for this setting.
   */
  void string_restricted_f(config_map_t &vars, const std::string &name, std::string &destination, const std::vector<std::string_view> &allowed_vals) {
    std::string temp;
    string_f(vars, name, temp);

    for (auto &allowed_val : allowed_vals) {
      if (temp == allowed_val) {
        destination = std::move(temp);
        return;
      }
    }
  }

  /**
   * @brief Parse a comma-separated string setting into a list.
   *
   * @param vars Configuration key-value map.
   * @param name Setting name.
   * @param output Parsed string list.
   */
  void string_list_f(config_map_t &vars, const std::string &name, std::vector<std::string> &output) {
    std::string temp;
    string_f(vars, name, temp);

    if (temp.empty()) {
      return;
    }

    output.clear();
    std::stringstream ss(temp);
    std::string item;
    while (std::getline(ss, item, ',')) {
      // Trim whitespace
      item.erase(0, item.find_first_not_of(" \t\r\n"));
      item.erase(item.find_last_not_of(" \t\r\n") + 1);
      if (!item.empty()) {
        output.push_back(item);
      }
    }
  }

  /**
   * @brief Consume a path setting and normalize it under the app data directory when relative.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void path_f(config_map_t &vars, const std::string &name, fs::path &destination) {
    // appdata needs to be retrieved once only
    static auto appdata = platf::appdata();

    std::string temp;
    string_f(vars, name, temp);

    if (!temp.empty()) {
      destination = temp;
    }

    if (destination.is_relative()) {
      destination = appdata / destination;
    }

    auto dir = destination;
    dir.remove_filename();

    // Ensure the directories exists
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
  }

  /**
   * @brief Consume a path setting and normalize it under the app data directory when relative.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void path_f(config_map_t &vars, const std::string &name, std::string &destination) {
    fs::path temp = destination;

    path_f(vars, name, temp);

    destination = temp.string();
  }

  /**
   * @brief Parse a decimal or hexadecimal integer configuration value.
   *
   * @param value Raw configuration value, optionally surrounded by quotes.
   * @return Parsed integer value.
   */
  int parse_config_integer(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"') {
      value = value.substr(1, value.size() - 2);
    }

    if (value.starts_with("0x"sv)) {
      return util::from_hex<int>(value.substr(2));
    }
    return static_cast<int>(util::from_view(value));
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void int_f(config_map_t &vars, const std::string &name, int &destination) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    destination = parse_config_integer(it->second);
    vars.erase(it);
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void int_f(config_map_t &vars, const std::string &name, std::optional<int> &destination) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    destination = parse_config_integer(it->second);
    vars.erase(it);
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   * @param f Converter applied to the raw configuration string.
   */
  template<class F>
  void int_f(config_map_t &vars, const std::string &name, int &destination, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      destination = f(tmp);
    }
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   * @param f Converter applied to the raw configuration string.
   */
  template<class F>
  void int_f(config_map_t &vars, const std::string &name, std::optional<int> &destination, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      destination = f(tmp);
    }
  }

  /**
   * @brief Consume an integer setting only when it falls inside an inclusive range.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   * @param range Inclusive range accepted for the parsed value.
   */
  void int_between_f(config_map_t &vars, const std::string &name, int &destination, const std::pair<int, int> &range) {
    int temp = destination;

    int_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      destination = temp;
    }
  }

  /**
   * @brief Convert common textual boolean forms to a boolean value.
   *
   * @param boolean Configuration string to classify as enabled or disabled.
   * @return True when the tested parser condition is met.
   */
  bool to_bool(std::string &boolean) {
    std::transform(std::begin(boolean), std::end(boolean), std::begin(boolean), [](unsigned char ch) {
      return (char) std::tolower(ch);
    });

    return boolean == "true"sv ||
           boolean == "yes"sv ||
           boolean == "enable"sv ||
           boolean == "enabled"sv ||
           boolean == "on"sv ||
           (std::find(std::begin(boolean), std::end(boolean), '1') != std::end(boolean));
  }

  /**
   * @brief Consume a boolean setting from the parsed configuration map.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void bool_f(config_map_t &vars, const std::string &name, bool &destination) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    destination = to_bool(tmp);
  }

  /**
   * @brief Consume a floating-point setting from the parsed configuration map.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void double_f(config_map_t &vars, const std::string &name, double &destination) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    char *c_str_p;
    auto val = std::strtod(tmp.c_str(), &c_str_p);

    if (c_str_p == tmp.c_str()) {
      return;
    }

    destination = val;
  }

  /**
   * @brief Consume a floating-point setting only when it falls inside an inclusive range.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   * @param range Inclusive range accepted for the parsed value.
   */
  void double_between_f(config_map_t &vars, const std::string &name, double &destination, const std::pair<double, double> &range) {
    double temp = destination;

    double_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      destination = temp;
    }
  }

  /**
   * @brief Consume a comma-separated or bracketed string list setting.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void list_string_f(config_map_t &vars, const std::string &name, std::vector<std::string> &destination) {
    std::string string;
    string_f(vars, name, string);

    if (string.empty()) {
      return;
    }

    destination.clear();

    auto begin = std::cbegin(string);
    if (*begin == '[') {
      ++begin;
    }

    begin = std::find_if_not(begin, std::cend(string), whitespace);
    if (begin == std::cend(string)) {
      return;
    }

    auto pos = begin;
    while (pos < std::cend(string)) {
      if (*pos == '[') {
        pos = skip_list(pos + 1, std::cend(string)) + 1;
      } else if (*pos == ']') {
        break;
      } else if (*pos == ',') {
        destination.emplace_back(begin, pos);
        pos = begin = std::find_if_not(pos + 1, std::cend(string), whitespace);
      } else {
        ++pos;
      }
    }

    if (pos != begin) {
      destination.emplace_back(begin, pos);
    }
  }

  /**
   * @brief Consume the JSON preparation-command list setting.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void list_prep_cmd_f(config_map_t &vars, const std::string &name, std::vector<prep_cmd_t> &destination) {
    std::string string;
    string_f(vars, name, string);

    std::stringstream jsonStream;

    // check if string is empty, i.e. when the value doesn't exist in the config file
    if (string.empty()) {
      return;
    }

    // We need to add a wrapping object to make it valid JSON, otherwise ptree cannot parse it.
    jsonStream << "{\"prep_cmd\":" << string << "}";

    boost::property_tree::ptree jsonTree;
    boost::property_tree::read_json(jsonStream, jsonTree);

    for (auto &[_, prep_cmd] : jsonTree.get_child("prep_cmd"s)) {
      auto do_cmd = prep_cmd.get_optional<std::string>("do"s);
      auto undo_cmd = prep_cmd.get_optional<std::string>("undo"s);
      auto elevated = prep_cmd.get_optional<bool>("elevated"s);

      destination.emplace_back(do_cmd.value_or(""), undo_cmd.value_or(""), elevated.value_or(false));
    }
  }

  /**
   * @brief Consume an integer list setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void list_int_f(config_map_t &vars, const std::string &name, std::vector<int> &destination) {
    std::vector<std::string> list;
    list_string_f(vars, name, list);

    // check if list is empty, i.e. when the value doesn't exist in the config file
    if (list.empty()) {
      return;
    }

    // The framerate list must be cleared before adding values from the file configuration.
    // If the list is not cleared, then the specified parameters do not affect the behavior of the sunshine server.
    // That is, if you set only 30 fps in the configuration file, it will not work because by default, during initialization the list includes 10, 30, 60, 90 and 120 fps.
    destination.clear();
    for (auto &el : list) {
      std::string_view val = el;

      // If value is something like: "756" instead of 756
      if (val.size() >= 2 && val[0] == '"') {
        val = val.substr(1, val.size() - 2);
      }

      int tmp;

      // If the integer is a hexadecimal
      if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
        tmp = util::from_hex<int>(val.substr(2));
      } else {
        tmp = (int) util::from_view(val);
      }
      destination.emplace_back(tmp);
    }
  }

  /**
   * @brief Consume an integer-pair list into a mapping table.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param destination Destination field updated when the setting exists and parses successfully.
   */
  void map_int_int_f(config_map_t &vars, const std::string &name, std::unordered_map<int, int> &destination) {
    std::vector<int> list;
    list_int_f(vars, name, list);

    // The list needs to be a multiple of 2
    if (list.size() % 2) {
      BOOST_LOG(warning) << "config: expected "sv << name << " to have a multiple of two elements --> not "sv << list.size();
      return;
    }

    int x = 0;
    while (x < list.size()) {
      auto key = list[x++];
      auto val = list[x++];

      destination.emplace(key, val);
    }
  }

  /**
   * @brief Apply single-character command-line flags to the global Sunshine flags bitset.
   *
   * @param line Configuration line being parsed.
   * @return 0 when all flags are recognized; -1 when at least one flag is unknown.
   */
  int apply_flags(const char *line) {
    int ret = 0;
    while (*line != '\0') {
      switch (*line) {
        case '0':
          config::sunshine.flags[config::flag::PIN_STDIN].flip();
          break;
        case '1':
          config::sunshine.flags[config::flag::FRESH_STATE].flip();
          break;
        case '2':
          config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE].flip();
          break;
        case 'p':
          config::sunshine.flags[config::flag::UPNP].flip();
          break;
        default:
          BOOST_LOG(warning) << "config: Unrecognized flag: ["sv << *line << ']' << std::endl;
          ret = -1;
      }

      ++line;
    }

    return ret;
  }

  /**
   * @brief Get supported gamepad options.
   *
   * @return Platform-supported gamepad backend names accepted by configuration.
   */
  const std::vector<std::string_view> &get_supported_gamepad_options() {
    static const auto gamepads = platf::supported_gamepads(nullptr);
    static const auto options = []() {
      std::vector<std::string_view> names;
      names.reserve(gamepads.size());
      for (const auto &gamepad : gamepads) {
        names.emplace_back(gamepad.name);
      }
      return names;
    }();
    return options;
  }

  /**
   * @brief Log parsed configuration entries and optionally record them as modified.
   */
  void log_config_settings(const config_map_t &vars, bool save) {
    for (auto &[name, val] : vars) {
      bool is_redacted = std::ranges::find(config::redacted_config, name) != config::redacted_config.end();

      BOOST_LOG(info) << "config: '"sv << name << "' = "sv << (is_redacted ? "[redacted]" : val);

      if (save) {
        modified_config_settings[name] = val;
      }
    }
  }

  bool cap_stream_resolution(int &width, int &height) {
    if (video.max_stream_width <= 0 || video.max_stream_height <= 0 || width <= 0 || height <= 0 ||
        (width <= video.max_stream_width && height <= video.max_stream_height)) {
      return false;
    }

    const double scale = std::min(
      (double) video.max_stream_width / width,
      (double) video.max_stream_height / height
    );
    // Encoders and decoders expect even dimensions
    const int capped_width = ((int) (width * scale)) & ~1;
    const int capped_height = ((int) (height * scale)) & ~1;
    BOOST_LOG(info) << "Capping stream resolution: "sv << width << 'x' << height
                    << " -> "sv << capped_width << 'x' << capped_height;
    width = capped_width;
    height = capped_height;
    return true;
  }

  /**
   * @brief Parse a `<width>x<height>` resolution string.
   *
   * @param value Resolution text such as `2560x1440`.
   * @param width Parsed width, written only on success.
   * @param height Parsed height, written only on success.
   * @return `true` when the string is a valid positive resolution.
   */
  bool parse_stream_resolution(const std::string &value, int &width, int &height) {
    const auto separator = value.find('x');
    if (separator == std::string::npos) {
      return false;
    }

    const auto *const begin = value.data();
    const auto *const end = begin + value.size();

    int parsed_width = 0;
    const auto [width_end, width_ec] = std::from_chars(begin, begin + separator, parsed_width);
    if (width_ec != std::errc {} || width_end != begin + separator || parsed_width <= 0) {
      return false;
    }

    int parsed_height = 0;
    const auto [height_end, height_ec] = std::from_chars(begin + separator + 1, end, parsed_height);
    if (height_ec != std::errc {} || height_end != end || parsed_height <= 0) {
      return false;
    }

    width = parsed_width;
    height = parsed_height;
    return true;
  }

  /**
   * @brief Apply parsed configuration entries to the global runtime configuration.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   */
  void apply_config(config_map_t &&vars) {
    log_config_settings(vars, true);

    int_f(vars, "qp", video.qp);
    int_between_f(vars, "hevc_mode", video.hevc_mode, {0, 3});
    int_between_f(vars, "av1_mode", video.av1_mode, {0, 3});
    int_f(vars, "min_threads", video.min_threads);

    std::string max_stream_resolution;
    string_f(vars, "max_stream_resolution", max_stream_resolution);
    if (!max_stream_resolution.empty() && !parse_stream_resolution(max_stream_resolution, video.max_stream_width, video.max_stream_height)) {
      BOOST_LOG(warning) << "max_stream_resolution must be formatted as <width>x<height>, e.g. 2560x1440: "sv << max_stream_resolution;
    }

    string_f(vars, "sw_preset", video.sw.sw_preset);
    if (!video.sw.sw_preset.empty()) {
      video.sw.svtav1_preset = sw::svtav1_preset_from_view(video.sw.sw_preset);
    }
    string_f(vars, "sw_tune", video.sw.sw_tune);

    int_between_f(vars, "nvenc_preset", video.nv.quality_preset, {1, 7});
    int_between_f(vars, "nvenc_vbv_increase", video.nv.vbv_percentage_increase, {0, 400});
    bool_f(vars, "nvenc_spatial_aq", video.nv.adaptive_quantization);
    generic_f(vars, "nvenc_twopass", video.nv.two_pass, nv::twopass_from_view);
    bool_f(vars, "nvenc_h264_cavlc", video.nv.h264_cavlc);
    generic_f(vars, "nvenc_split_encode", video.nv.split_frame_encoding, nv::split_encode_from_view);
    bool_f(vars, "nvenc_realtime_hags", video.nv_realtime_hags);
    bool_f(vars, "nvenc_opengl_vulkan_on_dxgi", video.nv_opengl_vulkan_on_dxgi);
    bool_f(vars, "nvenc_latency_over_power", video.nv_sunshine_high_power_mode);

#if !defined(__ANDROID__) && !defined(__APPLE__)
    video.nv_legacy.preset = nv::ffmpeg_preset_from_quality(video.nv.quality_preset);
    video.nv_legacy.multipass = video.nv.two_pass == nvenc::nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                video.nv.two_pass == nvenc::nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                                 NV_ENC_MULTI_PASS_DISABLED;
    video.nv_legacy.h264_coder = video.nv.h264_cavlc ? NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC : NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    video.nv_legacy.spatial_aq = video.nv.adaptive_quantization;
    video.nv_legacy.vbv_percentage_increase = video.nv.vbv_percentage_increase;
#endif

    int_f(vars, "qsv_preset", video.qsv.qsv_preset, qsv::preset_from_view);
    int_f(vars, "qsv_coder", video.qsv.qsv_cavlc, qsv::coder_from_view);
    bool_f(vars, "qsv_slow_hevc", video.qsv.qsv_slow_hevc);

    std::string quality;
    string_f(vars, "amd_quality", quality);
    if (!quality.empty()) {
      video.amd.amd_quality_h264 = amd::quality_from_view<amd::quality_h264_e>(quality, video.amd.amd_quality_h264);
      video.amd.amd_quality_hevc = amd::quality_from_view<amd::quality_hevc_e>(quality, video.amd.amd_quality_hevc);
      video.amd.amd_quality_av1 = amd::quality_from_view<amd::quality_av1_e>(quality, video.amd.amd_quality_av1);
    }

    std::string rc;
    string_f(vars, "amd_rc", rc);
    int_f(vars, "amd_coder", video.amd.amd_coder, amd::coder_from_view);
    if (!rc.empty()) {
      video.amd.amd_rc_h264 = amd::rc_from_view<amd::rc_h264_e>(rc, video.amd.amd_rc_h264);
      video.amd.amd_rc_hevc = amd::rc_from_view<amd::rc_hevc_e>(rc, video.amd.amd_rc_hevc);
      video.amd.amd_rc_av1 = amd::rc_from_view<amd::rc_av1_e>(rc, video.amd.amd_rc_av1);
    }

    std::string usage;
    string_f(vars, "amd_usage", usage);
    if (!usage.empty()) {
      video.amd.amd_usage_h264 = amd::usage_from_view<amd::usage_h264_e>(usage, video.amd.amd_usage_h264);
      video.amd.amd_usage_hevc = amd::usage_from_view<amd::usage_hevc_e>(usage, video.amd.amd_usage_hevc);
      video.amd.amd_usage_av1 = amd::usage_from_view<amd::usage_av1_e>(usage, video.amd.amd_usage_av1);
    }

    bool_f(vars, "amd_preanalysis", (bool &) video.amd.amd_preanalysis);
    bool_f(vars, "amd_vbaq", (bool &) video.amd.amd_vbaq);
    bool_f(vars, "amd_enforce_hrd", (bool &) video.amd.amd_enforce_hrd);
    {
      auto max_au_size = video.amd.amd_max_au_size;
      int_f(vars, "amd_max_au_size", max_au_size);
      if (!max_au_size || *max_au_size >= -1) {
        video.amd.amd_max_au_size = max_au_size;
      }
    }

    int_f(vars, "vt_coder", video.vt.vt_coder, vt::coder_from_view);
    int_f(vars, "vt_realtime", video.vt.vt_realtime, vt::rt_from_view);

    std::string vaapi_quality;
    string_f(vars, "vaapi_quality", vaapi_quality);
    if (!vaapi_quality.empty()) {
      video.vaapi.vaapi_quality = vaapi::quality_from_view<vaapi::quality_e>(vaapi_quality, video.vaapi.vaapi_quality);
    }
    string_f(vars, "vaapi_rc", video.vaapi.vaapi_rc_str);
    if (!video.vaapi.vaapi_rc_str.empty()) {
      video.vaapi.vaapi_rc = vaapi::rc_from_view<vaapi::rc_e>(video.vaapi.vaapi_rc_str, video.vaapi.vaapi_rc);
    }
    bool_f(vars, "vaapi_blbrc", (bool &) video.vaapi.blbrc);
    bool_f(vars, "vaapi_strict_rc_buffer", video.vaapi.strict_rc_buffer);

    int_f(vars, "vk_tune", video.vk.tune);
    int_f(vars, "vk_rc_mode", video.vk.rc_mode);
    std::string vk_quality;
    string_f(vars, "vk_quality", vk_quality);
    static const std::unordered_map<std::string_view, int> vk_quality_map = {
      {"speed"sv, 1},
      {"balanced"sv, 2},
      {"quality"sv, 3}
    };
    if (auto it = vk_quality_map.find(vk_quality); it != vk_quality_map.end()) {
      video.vk.quality = it->second;
    }

    string_f(vars, "capture", video.capture);
    bool_f(vars, "fp16_sdr_gamma_encoded", video.fp16_sdr_gamma_encoded);
    string_f(vars, "encoder", video.encoder);
    string_f(vars, "adapter_name", video.adapter_name);
    string_f(vars, "output_name", video.output_name);

    generic_f(vars, "dd_configuration_option", video.dd.configuration_option, dd::config_option_from_view);
    generic_f(vars, "dd_resolution_option", video.dd.resolution_option, dd::resolution_option_from_view);
    string_f(vars, "dd_manual_resolution", video.dd.manual_resolution);
    generic_f(vars, "dd_refresh_rate_option", video.dd.refresh_rate_option, dd::refresh_rate_option_from_view);
    string_f(vars, "dd_manual_refresh_rate", video.dd.manual_refresh_rate);
    generic_f(vars, "dd_hdr_option", video.dd.hdr_option, dd::hdr_option_from_view);
    {
      int value = -1;
      int_between_f(vars, "dd_config_revert_delay", value, {0, std::numeric_limits<int>::max()});
      if (value >= 0) {
        video.dd.config_revert_delay = std::chrono::milliseconds {value};
      }
    }
    bool_f(vars, "dd_config_revert_on_disconnect", video.dd.config_revert_on_disconnect);
    generic_f(vars, "dd_mode_remapping", video.dd.mode_remapping, dd::mode_remapping_from_view);
    {
      int value = 0;
      int_between_f(vars, "dd_wa_hdr_toggle_delay", value, {0, 3000});
      video.dd.wa.hdr_toggle_delay = std::chrono::milliseconds {value};
    }

    int_f(vars, "max_bitrate", video.max_bitrate);
    double_between_f(vars, "minimum_fps_target", video.minimum_fps_target, {0.0, 1000.0});

    path_f(vars, "pkey", nvhttp.pkey);
    path_f(vars, "cert", nvhttp.cert);
    string_f(vars, "sunshine_name", nvhttp.sunshine_name);
    path_f(vars, "log_path", config::sunshine.log_file);
    path_f(vars, "file_state", nvhttp.file_state);

    // Must be run after "file_state"
    config::sunshine.credentials_file = config::nvhttp.file_state;
    path_f(vars, "credentials_file", config::sunshine.credentials_file);

    string_f(vars, "external_ip", nvhttp.external_ip);
    list_prep_cmd_f(vars, "global_prep_cmd", config::sunshine.prep_cmds);

    string_f(vars, "audio_sink", audio.sink);
    string_f(vars, "virtual_sink", audio.virtual_sink);
    bool_f(vars, "stream_audio", audio.stream);
    bool_f(vars, "install_steam_audio_drivers", audio.install_steam_drivers);
    bool_f(vars, "external_audio", audio.external_audio);

    string_restricted_f(vars, "origin_web_ui_allowed", nvhttp.origin_web_ui_allowed, {"pc"sv, "lan"sv, "wan"sv});

    // Parse CSRF allowed origins - always include defaults, then append user-configured origins
    std::vector<std::string> user_csrf_origins;
    string_list_f(vars, "csrf_allowed_origins", user_csrf_origins);

    // Start with default localhost variants
    sunshine.csrf_allowed_origins = {
      "https://localhost",
      "https://127.0.0.1",
      "https://[::1]"
    };

    // Validate and append user-configured origins
    bool csrf_invalid_config = false;
    for (const auto &origin : user_csrf_origins) {
      if (origin.size() > 8 && origin.starts_with("https://")) {
        sunshine.csrf_allowed_origins.push_back(origin);
      } else {
        csrf_invalid_config = true;
        BOOST_LOG(warning) << "Invalid 'csrf_allowed_origins' entry rejected: "sv << origin;
      }
    }
    if (csrf_invalid_config) {
      BOOST_LOG(warning) << "Please refer to: https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2configuration.html#csrf_allowed_origins"sv;
    }

    int to = -1;
    int_between_f(vars, "ping_timeout", to, {-1, std::numeric_limits<int>::max()});
    if (to != -1) {
      stream.ping_timeout = std::chrono::milliseconds(to);
    }

    int_between_f(vars, "lan_encryption_mode", stream.lan_encryption_mode, {0, 2});
    int_between_f(vars, "wan_encryption_mode", stream.wan_encryption_mode, {0, 2});
    int_between_f(vars, "packetsize", stream.packetsize, {0, PACKETSIZE_MAX});
    bool_f(vars, "dscp_tagging", stream.dscp_tagging);

    path_f(vars, "file_apps", stream.file_apps);
#ifndef __ANDROID__
    // TODO: Android can possibly support this
    if (!fs::exists(stream.file_apps.c_str())) {
      fs::copy_file(SUNSHINE_ASSETS_DIR "/apps.json", stream.file_apps);
      fs::permissions(
        stream.file_apps,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::add
      );
    }
#endif

    int_between_f(vars, "fec_percentage", stream.fec_percentage, {0, 100});

    map_int_int_f(vars, "keybindings"s, input.keybindings);

    // This config option will only be used by the UI
    // When editing in the config file itself, use "keybindings"
    input.key_rightalt_to_key_win = false;
    bool_f(vars, "key_rightalt_to_key_win", input.key_rightalt_to_key_win);

    if (input.key_rightalt_to_key_win) {
      input.keybindings.emplace(0xA5, 0x5B);
    }

    to = std::numeric_limits<int>::min();
    int_f(vars, "back_button_timeout", to);

    if (to > std::numeric_limits<int>::min()) {
      input.back_button_timeout = std::chrono::milliseconds {to};
    }

    double repeat_frequency {0};
    double_between_f(vars, "key_repeat_frequency", repeat_frequency, {0, std::numeric_limits<double>::max()});

    if (repeat_frequency > 0) {
      config::input.key_repeat_period = std::chrono::duration<double> {1 / repeat_frequency};
    }

    to = -1;
    int_f(vars, "key_repeat_delay", to);
    if (to >= 0) {
      input.key_repeat_delay = std::chrono::milliseconds {to};
    }

    string_restricted_f(vars, "gamepad_driver", input.gamepad_driver, {
                                                                        GAMEPAD_DRIVER_ALL,
                                                                        GAMEPAD_DRIVER_VIRTUALHID,
                                                                        GAMEPAD_DRIVER_VIGEMBUS,
                                                                        GAMEPAD_DRIVER_NONE,
                                                                      });
    string_restricted_f(vars, "gamepad"s, input.gamepad, get_supported_gamepad_options());
#ifdef _WIN32
    if (input.gamepad_driver == GAMEPAD_DRIVER_VIGEMBUS && input.gamepad != "auto"sv && input.gamepad != "x360"sv && input.gamepad != "ds4"sv) {
      BOOST_LOG(warning) << "Gamepad type '"sv << input.gamepad << "' is not supported by ViGEmBus; using automatic selection"sv;
      input.gamepad = "auto";
    }
#endif
    bool_f(vars, "ds4_back_as_touchpad_click", input.ds4_back_as_touchpad_click);
    bool_f(vars, "motion_as_ds4", input.motion_as_ds4);
    bool_f(vars, "touchpad_as_ds4", input.touchpad_as_ds4);
    bool_f(vars, "virtualhid_randomize_mac", input.virtualhid_randomize_mac);

    bool_f(vars, "mouse", input.mouse);
    bool_f(vars, "keyboard", input.keyboard);
    bool_f(vars, "controller", input.controller);

    bool_f(vars, "always_send_scancodes", input.always_send_scancodes);

    bool_f(vars, "high_resolution_scrolling", input.high_resolution_scrolling);
    bool_f(vars, "native_pen_touch", input.native_pen_touch);

    bool_f(vars, "notify_pre_releases", sunshine.notify_pre_releases);
    bool_f(vars, "system_tray", sunshine.system_tray);

    int port = sunshine.port;
    int_between_f(vars, "port"s, port, {1024 + nvhttp::PORT_HTTPS, 65535 - rtsp_stream::RTSP_SETUP_PORT});
    sunshine.port = (std::uint16_t) port;

    // Now that we have the port, add web UI port-specific origins to CSRF allowed list
    // Web UI runs on port + 1 (PORT_HTTPS offset is 1 for confighttp)
    const unsigned short web_ui_port = sunshine.port + 1;
    sunshine.csrf_allowed_origins.push_back(std::format("https://localhost:{}", web_ui_port));
    sunshine.csrf_allowed_origins.push_back(std::format("https://127.0.0.1:{}", web_ui_port));
    sunshine.csrf_allowed_origins.push_back(std::format("https://[::1]:{}", web_ui_port));

    string_restricted_f(vars, "address_family", sunshine.address_family, {"ipv4"sv, "both"sv});
    string_f(vars, "bind_address", sunshine.bind_address);

    bool upnp = false;
    bool_f(vars, "upnp"s, upnp);

    if (upnp) {
      config::sunshine.flags[config::flag::UPNP].flip();
    }

    string_restricted_f(vars, "locale", config::sunshine.locale, {
                                                                   "bg"sv,  // Bulgarian
                                                                   "cs"sv,  // Czech
                                                                   "de"sv,  // German
                                                                   "en"sv,  // English
                                                                   "en_GB"sv,  // English (UK)
                                                                   "en_US"sv,  // English (US)
                                                                   "es"sv,  // Spanish
                                                                   "fr"sv,  // French
                                                                   "hu"sv,  // Hungarian
                                                                   "it"sv,  // Italian
                                                                   "ja"sv,  // Japanese
                                                                   "ko"sv,  // Korean
                                                                   "pl"sv,  // Polish
                                                                   "pt"sv,  // Portuguese
                                                                   "pt_BR"sv,  // Portuguese (Brazilian)
                                                                   "ru"sv,  // Russian
                                                                   "sv"sv,  // Swedish
                                                                   "tr"sv,  // Turkish
                                                                   "uk"sv,  // Ukrainian
                                                                   "vi"sv,  // Vietnamese
                                                                   "zh"sv,  // Chinese
                                                                   "zh_TW"sv,  // Chinese (Traditional)
                                                                 });

    std::string log_level_string;
    string_f(vars, "min_log_level", log_level_string);

    if (!log_level_string.empty()) {
      if (log_level_string == "verbose"sv) {
        sunshine.min_log_level = 0;
      } else if (log_level_string == "debug"sv) {
        sunshine.min_log_level = 1;
      } else if (log_level_string == "info"sv) {
        sunshine.min_log_level = 2;
      } else if (log_level_string == "warning"sv) {
        sunshine.min_log_level = 3;
      } else if (log_level_string == "error"sv) {
        sunshine.min_log_level = 4;
      } else if (log_level_string == "fatal"sv) {
        sunshine.min_log_level = 5;
      } else if (log_level_string == "none"sv) {
        sunshine.min_log_level = 6;
      } else {
        // accept digit directly
        auto val = log_level_string[0];
        if (val >= '0' && val < '7') {
          sunshine.min_log_level = val - '0';
        }
      }
    }

    auto it = vars.find("flags"s);
    if (it != std::end(vars)) {
      apply_flags(it->second.c_str());

      vars.erase(it);
    }

    if (sunshine.min_log_level <= 3) {
      for (auto &[var, _] : vars) {
        std::cout << "Warning: Unrecognized configurable option ["sv << var << ']' << std::endl;
      }
    }
  }

#ifdef SUNSHINE_TESTS
  /**
   * @brief Parse and apply serialized configuration text for unit tests.
   *
   * @param file_content Raw configuration text to parse and apply.
   */
  void apply_config_for_test(const std::string_view file_content) {
    apply_config(parse_config(file_content));
  }
#endif

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   */
  int parse(int argc, char *argv[]) {
    config_map_t cmd_vars;
#ifdef _WIN32
    bool shortcut_launch = false;
    bool service_admin_launch = false;
#endif

    for (auto x = 1; x < argc; ++x) {
      auto line = argv[x];

      if (line == "--help"sv) {
        logging::print_help(*argv);
        return 1;
      }
#ifdef _WIN32
      else if (line == "--shortcut"sv) {
        shortcut_launch = true;
      } else if (line == "--shortcut-admin"sv) {
        service_admin_launch = true;
      }
#endif
      else if (*line == '-') {
        if (*(line + 1) == '-') {
          sunshine.cmd.name = line + 2;
          sunshine.cmd.argc = argc - x - 1;
          sunshine.cmd.argv = argv + x + 1;

          break;
        }
        if (apply_flags(line + 1)) {
          logging::print_help(*argv);
          return -1;
        }
      } else {
        auto line_end = line + strlen(line);

        auto pos = std::find(line, line_end, '=');
        if (pos == line_end) {
          sunshine.config_file = line;
        } else {
          TUPLE_EL(var, 1, parse_option(line, line_end));
          if (!var) {
            logging::print_help(*argv);
            return -1;
          }

          TUPLE_EL_REF(name, 0, *var);

          auto it = cmd_vars.find(name);
          if (it != std::end(cmd_vars)) {
            cmd_vars.erase(it);
          }

          cmd_vars.try_emplace(std::move(var->first), std::move(var->second));
        }
      }
    }

    bool config_loaded = false;
    try {
      // Create appdata folder if it does not exist
      file_handler::make_directory(platf::appdata().string());

      // Create empty config file if it does not exist
      if (!fs::exists(sunshine.config_file)) {
        std::ofstream {sunshine.config_file};
      }

      // Read config file
      auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));

      for (auto &[name, value] : cmd_vars) {
        vars.insert_or_assign(name, std::move(value));
      }

      // Apply the config. Note: This will try to create any paths
      // referenced in the config, so we may receive exceptions if
      // the path is incorrect or inaccessible.
      apply_config(std::move(vars));
      config_loaded = true;
    } catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    } catch (const boost::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    }

#ifdef _WIN32
    // UCRT64 raises an access denied exception if launching from the shortcut
    // as non-admin and the config folder is not yet present; we can defer
    // so that service instance will do the work instead.

    if (!config_loaded && !shortcut_launch) {
      BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
      std::this_thread::sleep_for(10s);
#else
    if (!config_loaded) {
#endif
      return -1;
    }

#ifdef _WIN32
    // We have to wait until the config is loaded to handle these launches,
    // because we need to have the correct base port loaded in our config.
    // Exception: UCRT64 shortcut_launch instances may have no config loaded due to
    // insufficient permissions to create folder; port defaults will be acceptable.
    if (service_admin_launch) {
      // This is a relaunch as admin to start the service
      service_ctrl::start_service();

      // Always return 1 to ensure Sunshine doesn't start normally
      return 1;
    }
    if (shortcut_launch) {
      if (!service_ctrl::is_service_running()) {
        // If the service isn't running, relaunch ourselves as admin to start it
        std::array<WCHAR, MAX_PATH> executable;
        GetModuleFileNameW(nullptr, executable.data(), executable.size());

        SHELLEXECUTEINFOW shell_exec_info {};
        shell_exec_info.cbSize = sizeof(shell_exec_info);
        shell_exec_info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS;
        shell_exec_info.lpVerb = L"runas";
        shell_exec_info.lpFile = executable.data();
        shell_exec_info.lpParameters = L"--shortcut-admin";
        shell_exec_info.nShow = SW_NORMAL;
        if (!ShellExecuteExW(&shell_exec_info)) {
          auto winerr = GetLastError();
          BOOST_LOG(error) << "Failed executing shell command: " << winerr << std::endl;
          return 1;
        }

        // Wait for the elevated process to finish starting the service
        WaitForSingleObject(shell_exec_info.hProcess, INFINITE);
        CloseHandle(shell_exec_info.hProcess);

        // Wait for the UI to be ready for connections
        service_ctrl::wait_for_ui_ready();
      }

      // Always return 1 to ensure Sunshine doesn't start normally
      return 1;
    }
#endif

    return 0;
  }
}  // namespace config
