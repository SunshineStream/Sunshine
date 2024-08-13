/**
 * @file src/config.cpp
 * @brief Definitions for the configuration of Sunshine.
 */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "config.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "logging.h"
#include "server/nvhttp.h"
#include "server/rtsp.h"
#include "utility.h"

#include "platform/common.h"

#ifdef _WIN32
  #include <shellapi.h>
#endif

#ifndef __APPLE__
  // For NVENC legacy constants
  #include <ffnvcodec/nvEncodeAPI.h>
#endif

namespace fs = std::filesystem;
using namespace std::literals;

#define CA_DIR "credentials"
#define PRIVATE_KEY_FILE CA_DIR "/cakey.pem"
#define CERTIFICATE_FILE CA_DIR "/cacert.pem"

#define APPS_JSON_PATH platf::appdata().string() + "/apps.json"
namespace config {

  namespace nv {

    nvenc::nvenc_two_pass
    twopass_from_view(const std::string_view &preset) {
      if (preset == "disabled") return nvenc::nvenc_two_pass::disabled;
      if (preset == "quarter_res") return nvenc::nvenc_two_pass::quarter_resolution;
      if (preset == "full_res") return nvenc::nvenc_two_pass::full_resolution;
      BOOST_LOG(warning) << "config: unknown nvenc_twopass value: " << preset;
      return nvenc::nvenc_two_pass::quarter_resolution;
    }

  }  // namespace nv

  namespace amd {
#ifdef __APPLE__
  // values accurate as of 27/12/2022, but aren't strictly necessary for MacOS build
  #define AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED 100
  #define AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY 30
  #define AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED 70
  #define AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED 10
  #define AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY 0
  #define AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED 5
  #define AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED 1
  #define AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY 2
  #define AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED 0
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP 0
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR 3
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR 2
  #define AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR 1
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP 0
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR 3
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR 2
  #define AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR 1
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP 0
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR 1
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR 2
  #define AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR 3
  #define AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING 0
  #define AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY 1
  #define AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY 2
  #define AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM 3
  #define AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY 5
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING 0
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY 1
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY 2
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM 3
  #define AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY 5
  #define AMF_VIDEO_ENCODER_USAGE_TRANSCODING 0
  #define AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY 1
  #define AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY 2
  #define AMF_VIDEO_ENCODER_USAGE_WEBCAM 3
  #define AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY 5
  #define AMF_VIDEO_ENCODER_UNDEFINED 0
  #define AMF_VIDEO_ENCODER_CABAC 1
  #define AMF_VIDEO_ENCODER_CALV 2
#else
  #include <AMF/components/VideoEncoderAV1.h>
  #include <AMF/components/VideoEncoderHEVC.h>
  #include <AMF/components/VideoEncoderVCE.h>
#endif

    enum class quality_av1_e : int {
      speed = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class quality_hevc_e : int {
      speed = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class quality_h264_e : int {
      speed = AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class rc_av1_e : int {
      cbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    enum class rc_hevc_e : int {
      cbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    enum class rc_h264_e : int {
      cbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    enum class usage_av1_e : int {
      transcoding = AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum class usage_hevc_e : int {
      transcoding = AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum class usage_h264_e : int {
      transcoding = AMF_VIDEO_ENCODER_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum coder_e : int {
      _auto = AMF_VIDEO_ENCODER_UNDEFINED,  ///< Auto
      cabac = AMF_VIDEO_ENCODER_CABAC,  ///< CABAC
      cavlc = AMF_VIDEO_ENCODER_CALV  ///< CAVLC
    };

    template <class T>
    std::optional<int>
    quality_from_view(const std::string_view &quality_type, const std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (quality_type == #x##sv) return (int) T::x
      _CONVERT_(balanced);
      _CONVERT_(quality);
      _CONVERT_(speed);
#undef _CONVERT_
      return original;
    }

    template <class T>
    std::optional<int>
    rc_from_view(const std::string_view &rc, const std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (rc == #x##sv) return (int) T::x
      _CONVERT_(cbr);
      _CONVERT_(cqp);
      _CONVERT_(vbr_latency);
      _CONVERT_(vbr_peak);
#undef _CONVERT_
      return original;
    }

    template <class T>
    std::optional<int>
    usage_from_view(const std::string_view &usage, const std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (usage == #x##sv) return (int) T::x
      _CONVERT_(lowlatency);
      _CONVERT_(lowlatency_high_quality);
      _CONVERT_(transcoding);
      _CONVERT_(ultralowlatency);
      _CONVERT_(webcam);
#undef _CONVERT_
      return original;
    }

    int
    coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) return _auto;
      if (coder == "cabac"sv || coder == "ac"sv) return cabac;
      if (coder == "cavlc"sv || coder == "vlc"sv) return cavlc;

      return _auto;
    }
  }  // namespace amd

  namespace qsv {
    enum preset_e : int {
      veryslow = 1,  ///< veryslow preset
      slower = 2,  ///< slower preset
      slow = 3,  ///< slow preset
      medium = 4,  ///< medium preset
      fast = 5,  ///< fast preset
      faster = 6,  ///< faster preset
      veryfast = 7  ///< veryfast preset
    };

    enum cavlc_e : int {
      _auto = false,  ///< Auto
      enabled = true,  ///< Enabled
      disabled = false  ///< Disabled
    };

    std::optional<int>
    preset_from_view(const std::string_view &preset) {
#define _CONVERT_(x) \
  if (preset == #x##sv) return x
      _CONVERT_(veryslow);
      _CONVERT_(slower);
      _CONVERT_(slow);
      _CONVERT_(medium);
      _CONVERT_(fast);
      _CONVERT_(faster);
      _CONVERT_(veryfast);
#undef _CONVERT_
      return std::nullopt;
    }

    std::optional<int>
    coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) return _auto;
      if (coder == "cabac"sv || coder == "ac"sv) return disabled;
      if (coder == "cavlc"sv || coder == "vlc"sv) return enabled;
      return std::nullopt;
    }

  }  // namespace qsv

  namespace vt {

    enum coder_e : int {
      _auto = 0,  ///< Auto
      cabac,  ///< CABAC
      cavlc  ///< CAVLC
    };

    int
    coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) return _auto;
      if (coder == "cabac"sv || coder == "ac"sv) return cabac;
      if (coder == "cavlc"sv || coder == "vlc"sv) return cavlc;

      return -1;
    }

    int
    allow_software_from_view(const std::string_view &software) {
      if (software == "allowed"sv || software == "forced") return 1;

      return 0;
    }

    int
    force_software_from_view(const std::string_view &software) {
      if (software == "forced") return 1;

      return 0;
    }

    int
    rt_from_view(const std::string_view &rt) {
      if (rt == "disabled" || rt == "off" || rt == "0") return 0;

      return 1;
    }

  }  // namespace vt

  namespace sw {
    int
    svtav1_preset_from_view(const std::string_view &preset) {
#define _CONVERT_(x, y) \
  if (preset == #x##sv) return y
      _CONVERT_(veryslow, 1);
      _CONVERT_(slower, 2);
      _CONVERT_(slow, 4);
      _CONVERT_(medium, 5);
      _CONVERT_(fast, 7);
      _CONVERT_(faster, 9);
      _CONVERT_(veryfast, 10);
      _CONVERT_(superfast, 11);
      _CONVERT_(ultrafast, 12);
#undef _CONVERT_
      return 11;  // Default to superfast
    }
  }  // namespace sw

  video_t video {
    28,  // qp

    0,  // hevc_mode
    0,  // av1_mode

    1,  // min_fps_factor
    2,  // min_threads
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
      (int) amd::usage_h264_e::ultralowlatency,  // usage (h264)
      (int) amd::usage_hevc_e::ultralowlatency,  // usage (hevc)
      (int) amd::usage_av1_e::ultralowlatency,  // usage (av1)
      (int) amd::rc_h264_e::vbr_latency,  // rate control (h264)
      (int) amd::rc_hevc_e::vbr_latency,  // rate control (hevc)
      (int) amd::rc_av1_e::vbr_latency,  // rate control (av1)
      0,  // enforce_hrd
      (int) amd::quality_h264_e::balanced,  // quality (h264)
      (int) amd::quality_hevc_e::balanced,  // quality (hevc)
      (int) amd::quality_av1_e::balanced,  // quality (av1)
      0,  // preanalysis
      1,  // vbaq
      (int) amd::coder_e::_auto,  // coder
    },  // amd

    {
      0,
      0,
      1,
      -1,
    },  // vt

    {},  // capture
    {},  // encoder
    {},  // adapter_name
    {},  // output_name
  };

  audio_t audio {
    {},  // audio_sink
    {},  // virtual_sink
    true,  // install_steam_drivers
  };

  stream_t stream {
    10s,  // ping_timeout

    APPS_JSON_PATH,

    20,  // fecPercentage
    1,  // channels

    ENCRYPTION_MODE_NEVER,  // lan_encryption_mode
    ENCRYPTION_MODE_OPPORTUNISTIC,  // wan_encryption_mode
  };

  nvhttp_t nvhttp {
    "lan",  // origin web manager

    PRIVATE_KEY_FILE,
    CERTIFICATE_FILE,

    boost::asio::ip::host_name(),  // sunshine_name,
    "sunshine_state.json"s,  // file_state
    {},  // external_ip
  };

  input_t input {
    {
      { 0x10, 0xA0 },
      { 0x11, 0xA2 },
      { 0x12, 0xA4 },
    },
    -1ms,  // back_button_timeout
    500ms,  // key_repeat_delay
    std::chrono::duration<double> { 1 / 24.9 },  // key_repeat_period

    {
      platf::supported_gamepads(nullptr).front().name.data(),
      platf::supported_gamepads(nullptr).front().name.size(),
    },  // Default gamepad
    true,  // back as touchpad click enabled (manual DS4 only)
    true,  // client gamepads with motion events are emulated as DS4
    true,  // client gamepads with touchpads are emulated as DS4

    true,  // keyboard enabled
    true,  // mouse enabled
    true,  // controller enabled
    true,  // always send scancodes
    true,  // high resolution scrolling
    true,  // native pen/touch support
  };

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
    platf::appdata().string() + "/sunshine.log",  // log file
    false,  // notify_pre_releases
    {},  // prep commands
  };

  bool
  endline(char ch) {
    return ch == '\r' || ch == '\n';
  }

  bool
  space_tab(char ch) {
    return ch == ' ' || ch == '\t';
  }

  bool
  whitespace(char ch) {
    return space_tab(ch) || endline(ch);
  }

  std::string
  to_string(const char *begin, const char *end) {
    std::string result;

    KITTY_WHILE_LOOP(auto pos = begin, pos != end, {
      auto comment = std::find(pos, end, '#');
      auto endl = std::find_if(comment, end, endline);

      result.append(pos, comment);

      pos = endl;
    })

    return result;
  }

  template <class It>
  It
  skip_list(It skipper, It end) {
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
      if (endl == end) {
        std::cout << "Warning: Config option ["sv << to_string(begin, end_name) << "] Missing ']'"sv;

        return std::make_pair(endl, std::nullopt);
      }
    }

    return std::make_pair(
      endl,
      std::make_pair(to_string(begin, end_name), to_string(begin_val, endl)));
  }

  std::unordered_map<std::string, std::string>
  parse_config(const std::string_view &file_content) {
    std::unordered_map<std::string, std::string> vars;

    auto pos = std::begin(file_content);
    auto end = std::end(file_content);

    while (pos < end) {
      // auto newline = std::find_if(pos, end, [](auto ch) { return ch == '\n' || ch == '\r'; });
      TUPLE_2D(endl, var, parse_option(pos, end));

      pos = endl;
      if (pos != end) {
        pos += (*pos == '\r') ? 2 : 1;
      }

      if (!var) {
        continue;
      }

      vars.emplace(std::move(*var));
    }

    return vars;
  }

  void
  string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    auto it = vars.find(name);
    if (it == std::end(vars)) {
      return;
    }

    input = std::move(it->second);

    vars.erase(it);
  }

  template <typename T, typename F>
  void
  generic_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, T &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  void
  string_restricted_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input, const std::vector<std::string_view> &allowed_vals) {
    std::string temp;
    string_f(vars, name, temp);

    for (auto &allowed_val : allowed_vals) {
      if (temp == allowed_val) {
        input = std::move(temp);
        return;
      }
    }
  }

  void
  path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, fs::path &input) {
    // appdata needs to be retrieved once only
    static auto appdata = platf::appdata();

    std::string temp;
    string_f(vars, name, temp);

    if (!temp.empty()) {
      input = temp;
    }

    if (input.is_relative()) {
      input = appdata / input;
    }

    auto dir = input;
    dir.remove_filename();

    // Ensure the directories exists
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
  }

  void
  path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    fs::path temp = input;

    path_f(vars, name, temp);

    input = temp.string();
  }

  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    std::string_view val = it->second;

    // If value is something like: "756" instead of 756
    if (val.size() >= 2 && val[0] == '"') {
      val = val.substr(1, val.size() - 2);
    }

    // If that integer is in hexadecimal
    if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
      input = util::from_hex<int>(val.substr(2));
    }
    else {
      input = util::from_view(val);
    }

    vars.erase(it);
  }

  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    std::string_view val = it->second;

    // If value is something like: "756" instead of 756
    if (val.size() >= 2 && val[0] == '"') {
      val = val.substr(1, val.size() - 2);
    }

    // If that integer is in hexadecimal
    if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
      input = util::from_hex<int>(val.substr(2));
    }
    else {
      input = util::from_view(val);
    }

    vars.erase(it);
  }

  template <class F>
  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  template <class F>
  void
  int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  void
  int_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, const std::pair<int, int> &range) {
    int temp = input;

    int_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  bool
  to_bool(std::string &boolean) {
    std::for_each(std::begin(boolean), std::end(boolean), [](char ch) { return (char) std::tolower(ch); });

    return boolean == "true"sv ||
           boolean == "yes"sv ||
           boolean == "enable"sv ||
           boolean == "enabled"sv ||
           boolean == "on"sv ||
           (std::find(std::begin(boolean), std::end(boolean), '1') != std::end(boolean));
  }

  void
  bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    input = to_bool(tmp);
  }

  void
  double_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input) {
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

    input = val;
  }

  void
  double_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input, const std::pair<double, double> &range) {
    double temp = input;

    double_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  void
  list_string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &input) {
    std::string string;
    string_f(vars, name, string);

    if (string.empty()) {
      return;
    }

    input.clear();

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
      }
      else if (*pos == ']') {
        break;
      }
      else if (*pos == ',') {
        input.emplace_back(begin, pos);
        pos = begin = std::find_if_not(pos + 1, std::cend(string), whitespace);
      }
      else {
        ++pos;
      }
    }

    if (pos != begin) {
      input.emplace_back(begin, pos);
    }
  }

  void
  list_prep_cmd_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<prep_cmd_t> &input) {
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

      input.emplace_back(do_cmd.value_or(""), undo_cmd.value_or(""), elevated.value_or(false));
    }
  }

  void
  list_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<int> &input) {
    std::vector<std::string> list;
    list_string_f(vars, name, list);

    // check if list is empty, i.e. when the value doesn't exist in the config file
    if (list.empty()) {
      return;
    }

    // The framerate list must be cleared before adding values from the file configuration.
    // If the list is not cleared, then the specified parameters do not affect the behavior of the sunshine server.
    // That is, if you set only 30 fps in the configuration file, it will not work because by default, during initialization the list includes 10, 30, 60, 90 and 120 fps.
    input.clear();
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
      }
      else {
        tmp = util::from_view(val);
      }
      input.emplace_back(tmp);
    }
  }

  void
  map_int_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::unordered_map<int, int> &input) {
    std::vector<int> list;
    list_int_f(vars, name, list);

    // The list needs to be a multiple of 2
    if (list.size() % 2) {
      std::cout << "Warning: expected "sv << name << " to have a multiple of two elements --> not "sv << list.size() << std::endl;
      return;
    }

    int x = 0;
    while (x < list.size()) {
      auto key = list[x++];
      auto val = list[x++];

      input.emplace(key, val);
    }
  }

  int
  apply_flags(const char *line) {
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
          std::cout << "Warning: Unrecognized flag: ["sv << *line << ']' << std::endl;
          ret = -1;
      }

      ++line;
    }

    return ret;
  }

  std::vector<std::string_view> &
  get_supported_gamepad_options() {
    const auto options = platf::supported_gamepads(nullptr);
    static std::vector<std::string_view> opts {};
    opts.reserve(options.size());
    for (auto &opt : options) {
      opts.emplace_back(opt.name);
    }
    return opts;
  }

  void
  apply_config(std::unordered_map<std::string, std::string> &&vars) {
    if (!fs::exists(stream.file_apps.c_str())) {
      fs::copy_file(SUNSHINE_ASSETS_DIR "/apps.json", stream.file_apps);
    }

    for (auto &[name, val] : vars) {
      std::cout << "["sv << name << "] -- ["sv << val << ']' << std::endl;
    }

    int_f(vars, "qp", video.qp);
    int_f(vars, "min_threads", video.min_threads);
    int_between_f(vars, "hevc_mode", video.hevc_mode, { 0, 3 });
    int_between_f(vars, "av1_mode", video.av1_mode, { 0, 3 });
    string_f(vars, "sw_preset", video.sw.sw_preset);
    if (!video.sw.sw_preset.empty()) {
      video.sw.svtav1_preset = sw::svtav1_preset_from_view(video.sw.sw_preset);
    }
    string_f(vars, "sw_tune", video.sw.sw_tune);

    int_between_f(vars, "nvenc_preset", video.nv.quality_preset, { 1, 7 });
    int_between_f(vars, "nvenc_vbv_increase", video.nv.vbv_percentage_increase, { 0, 400 });
    bool_f(vars, "nvenc_spatial_aq", video.nv.adaptive_quantization);
    generic_f(vars, "nvenc_twopass", video.nv.two_pass, nv::twopass_from_view);
    bool_f(vars, "nvenc_h264_cavlc", video.nv.h264_cavlc);
    bool_f(vars, "nvenc_realtime_hags", video.nv_realtime_hags);
    bool_f(vars, "nvenc_opengl_vulkan_on_dxgi", video.nv_opengl_vulkan_on_dxgi);
    bool_f(vars, "nvenc_latency_over_power", video.nv_sunshine_high_power_mode);

#ifndef __APPLE__
    video.nv_legacy.preset = video.nv.quality_preset + 11;
    video.nv_legacy.multipass = video.nv.two_pass == nvenc::nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                video.nv.two_pass == nvenc::nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                                 NV_ENC_MULTI_PASS_DISABLED;
    video.nv_legacy.h264_coder = video.nv.h264_cavlc ? NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC : NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    video.nv_legacy.aq = video.nv.adaptive_quantization;
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

    int_f(vars, "vt_coder", video.vt.vt_coder, vt::coder_from_view);
    int_f(vars, "vt_software", video.vt.vt_allow_sw, vt::allow_software_from_view);
    int_f(vars, "vt_software", video.vt.vt_require_sw, vt::force_software_from_view);
    int_f(vars, "vt_realtime", video.vt.vt_realtime, vt::rt_from_view);

    string_f(vars, "capture", video.capture);
    string_f(vars, "encoder", video.encoder);
    string_f(vars, "adapter_name", video.adapter_name);
    string_f(vars, "output_name", video.output_name);
    int_between_f(vars, "min_fps_factor", video.min_fps_factor, { 1, 3 });

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
    bool_f(vars, "install_steam_audio_drivers", audio.install_steam_drivers);

    string_restricted_f(vars, "origin_web_ui_allowed", nvhttp.origin_web_ui_allowed, { "pc"sv, "lan"sv, "wan"sv });

    int to = -1;
    int_between_f(vars, "ping_timeout", to, { -1, std::numeric_limits<int>::max() });
    if (to != -1) {
      stream.ping_timeout = std::chrono::milliseconds(to);
    }

    int_between_f(vars, "channels", stream.channels, { 1, std::numeric_limits<int>::max() });

    int_between_f(vars, "lan_encryption_mode", stream.lan_encryption_mode, { 0, 2 });
    int_between_f(vars, "wan_encryption_mode", stream.wan_encryption_mode, { 0, 2 });

    path_f(vars, "file_apps", stream.file_apps);
    int_between_f(vars, "fec_percentage", stream.fec_percentage, { 1, 255 });

    map_int_int_f(vars, "keybindings"s, input.keybindings);

    // This config option will only be used by the UI
    // When editing in the config file itself, use "keybindings"
    bool map_rightalt_to_win = false;
    bool_f(vars, "key_rightalt_to_key_win", map_rightalt_to_win);

    if (map_rightalt_to_win) {
      input.keybindings.emplace(0xA5, 0x5B);
    }

    to = std::numeric_limits<int>::min();
    int_f(vars, "back_button_timeout", to);

    if (to > std::numeric_limits<int>::min()) {
      input.back_button_timeout = std::chrono::milliseconds { to };
    }

    double repeat_frequency { 0 };
    double_between_f(vars, "key_repeat_frequency", repeat_frequency, { 0, std::numeric_limits<double>::max() });

    if (repeat_frequency > 0) {
      config::input.key_repeat_period = std::chrono::duration<double> { 1 / repeat_frequency };
    }

    to = -1;
    int_f(vars, "key_repeat_delay", to);
    if (to >= 0) {
      input.key_repeat_delay = std::chrono::milliseconds { to };
    }

    string_restricted_f(vars, "gamepad"s, input.gamepad, get_supported_gamepad_options());
    bool_f(vars, "ds4_back_as_touchpad_click", input.ds4_back_as_touchpad_click);
    bool_f(vars, "motion_as_ds4", input.motion_as_ds4);
    bool_f(vars, "touchpad_as_ds4", input.touchpad_as_ds4);

    bool_f(vars, "mouse", input.mouse);
    bool_f(vars, "keyboard", input.keyboard);
    bool_f(vars, "controller", input.controller);

    bool_f(vars, "always_send_scancodes", input.always_send_scancodes);

    bool_f(vars, "high_resolution_scrolling", input.high_resolution_scrolling);
    bool_f(vars, "native_pen_touch", input.native_pen_touch);

    bool_f(vars, "notify_pre_releases", sunshine.notify_pre_releases);

    int port = sunshine.port;
    int_between_f(vars, "port"s, port, { 1024 + nvhttp::PORT_HTTPS, 65535 - rtsp_stream::RTSP_SETUP_PORT });
    sunshine.port = (std::uint16_t) port;

    string_restricted_f(vars, "address_family", sunshine.address_family, { "ipv4"sv, "both"sv });

    bool upnp = false;
    bool_f(vars, "upnp"s, upnp);

    if (upnp) {
      config::sunshine.flags[config::flag::UPNP].flip();
    }

    string_restricted_f(vars, "locale", config::sunshine.locale, {
                                                                   "de"sv,  // German
                                                                   "en"sv,  // English
                                                                   "en_GB"sv,  // English (UK)
                                                                   "en_US"sv,  // English (US)
                                                                   "es"sv,  // Spanish
                                                                   "fr"sv,  // French
                                                                   "it"sv,  // Italian
                                                                   "ja"sv,  // Japanese
                                                                   "pt"sv,  // Portuguese
                                                                   "ru"sv,  // Russian
                                                                   "sv"sv,  // Swedish
                                                                   "tr"sv,  // Turkish
                                                                   "zh"sv,  // Chinese
                                                                 });

    std::string log_level_string;
    string_f(vars, "min_log_level", log_level_string);

    if (!log_level_string.empty()) {
      if (log_level_string == "verbose"sv) {
        sunshine.min_log_level = 0;
      }
      else if (log_level_string == "debug"sv) {
        sunshine.min_log_level = 1;
      }
      else if (log_level_string == "info"sv) {
        sunshine.min_log_level = 2;
      }
      else if (log_level_string == "warning"sv) {
        sunshine.min_log_level = 3;
      }
      else if (log_level_string == "error"sv) {
        sunshine.min_log_level = 4;
      }
      else if (log_level_string == "fatal"sv) {
        sunshine.min_log_level = 5;
      }
      else if (log_level_string == "none"sv) {
        sunshine.min_log_level = 6;
      }
      else {
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

  int
  parse(int argc, char *argv[]) {
    std::unordered_map<std::string, std::string> cmd_vars;
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
      }
      else if (line == "--shortcut-admin"sv) {
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
      }
      else {
        auto line_end = line + strlen(line);

        auto pos = std::find(line, line_end, '=');
        if (pos == line_end) {
          sunshine.config_file = line;
        }
        else {
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

          cmd_vars.emplace(std::move(*var));
        }
      }
    }

    bool config_loaded = false;
    try {
      // Create appdata folder if it does not exist
      file_handler::make_directory(platf::appdata().string());

      // Create empty config file if it does not exist
      if (!fs::exists(sunshine.config_file)) {
        std::ofstream { sunshine.config_file };
      }

      // Read config file
      auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));

      for (auto &[name, value] : cmd_vars) {
        vars.insert_or_assign(std::move(name), std::move(value));
      }

      // Apply the config. Note: This will try to create any paths
      // referenced in the config, so we may receive exceptions if
      // the path is incorrect or inaccessible.
      apply_config(std::move(vars));
      config_loaded = true;
    }
    catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    }
    catch (const boost::filesystem::filesystem_error &err) {
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
    else if (shortcut_launch) {
      if (!service_ctrl::is_service_running()) {
        // If the service isn't running, relaunch ourselves as admin to start it
        WCHAR executable[MAX_PATH];
        GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable));

        SHELLEXECUTEINFOW shell_exec_info {};
        shell_exec_info.cbSize = sizeof(shell_exec_info);
        shell_exec_info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS;
        shell_exec_info.lpVerb = L"runas";
        shell_exec_info.lpFile = executable;
        shell_exec_info.lpParameters = L"--shortcut-admin";
        shell_exec_info.nShow = SW_NORMAL;
        if (!ShellExecuteExW(&shell_exec_info)) {
          auto winerr = GetLastError();
          std::cout << "Error: ShellExecuteEx() failed:"sv << winerr << std::endl;
          return 1;
        }

        // Wait for the elevated process to finish starting the service
        WaitForSingleObject(shell_exec_info.hProcess, INFINITE);
        CloseHandle(shell_exec_info.hProcess);

        // Wait for the UI to be ready for connections
        service_ctrl::wait_for_ui_ready();
      }

      // Launch the web UI
      launch_ui();

      // Always return 1 to ensure Sunshine doesn't start normally
      return 1;
    }
#endif

    return 0;
  }
}  // namespace config
