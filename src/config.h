/**
 * @file src/config.h
 * @brief todo
 */
#pragma once

#include <bitset>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nvenc/nvenc_config.h"
#include "display_device/dd.h"

namespace config {
  struct video_t {
    // ffmpeg params
    int qp;  // higher == more compression and less quality

    int hevc_mode;
    int av1_mode;

    int min_threads;  // Minimum number of threads/slices for CPU encoding
    struct {
      std::string sw_preset;
      std::string sw_tune;
      std::optional<int> svtav1_preset;
    } sw;

    nvenc::nvenc_config nv;
    bool nv_realtime_hags;
    bool nv_opengl_vulkan_on_dxgi;
    bool nv_sunshine_high_power_mode;

    struct {
      int preset;
      int multipass;
      int h264_coder;
      int aq;
      int vbv_percentage_increase;
    } nv_legacy;

    struct {
      std::optional<int> qsv_preset;
      std::optional<int> qsv_cavlc;
      bool qsv_slow_hevc;
    } qsv;

    struct {
      std::optional<int> amd_usage_h264;
      std::optional<int> amd_usage_hevc;
      std::optional<int> amd_usage_av1;
      std::optional<int> amd_rc_h264;
      std::optional<int> amd_rc_hevc;
      std::optional<int> amd_rc_av1;
      std::optional<int> amd_enforce_hrd;
      std::optional<int> amd_quality_h264;
      std::optional<int> amd_quality_hevc;
      std::optional<int> amd_quality_av1;
      std::optional<int> amd_preanalysis;
      std::optional<int> amd_vbaq;
      int amd_coder;
    } amd;

    struct {
      int vt_allow_sw;
      int vt_require_sw;
      int vt_realtime;
      int vt_coder;
    } vt;

    std::string capture;
    std::string encoder;
    std::string adapter_name;
    std::string output_name;
  };

  struct audio_t {
    std::string sink;
    std::string virtual_sink;
    bool install_steam_drivers;
  };



  constexpr int ENCRYPTION_MODE_NEVER = 0;  // Never use video encryption, even if the client supports it
  constexpr int ENCRYPTION_MODE_OPPORTUNISTIC = 1;  // Use video encryption if available, but stream without it if not supported
  constexpr int ENCRYPTION_MODE_MANDATORY = 2;  // Always use video encryption and refuse clients that can't encrypt

  struct stream_t {
    std::chrono::milliseconds ping_timeout;

    std::string file_apps;

    int fec_percentage;

    // max unique instances of video and audio streams
    int channels;

    // Video encryption settings for LAN and WAN streams
    int lan_encryption_mode;
    int wan_encryption_mode;
  };

  struct nvhttp_t {
    // Could be any of the following values:
    // pc|lan|wan
    std::string origin_web_ui_allowed;

    std::string pkey;
    std::string cert;

    std::string sunshine_name;

    std::string file_state;

    std::string external_ip;
    std::vector<std::string> resolutions;
    std::vector<int> fps;
  };

  struct input_t {
    std::unordered_map<int, int> keybindings;

    std::chrono::milliseconds back_button_timeout;
    std::chrono::milliseconds key_repeat_delay;
    std::chrono::duration<double> key_repeat_period;

    std::string gamepad;
    bool ds4_back_as_touchpad_click;
    bool motion_as_ds4;
    bool touchpad_as_ds4;

    bool keyboard;
    bool mouse;
    bool controller;

    bool always_send_scancodes;

    bool high_resolution_scrolling;
    bool native_pen_touch;
  };

  namespace flag {
    enum flag_e : std::size_t {
      PIN_STDIN = 0,  // Read PIN from stdin instead of http
      FRESH_STATE,  // Do not load or save state
      FORCE_VIDEO_HEADER_REPLACE,  // force replacing headers inside video data
      UPNP,  // Try Universal Plug 'n Play
      CONST_PIN,  // Use "universal" pin
      FLAG_SIZE
    };
  }

  struct prep_cmd_t {
    prep_cmd_t(std::string &&do_cmd, std::string &&undo_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)), undo_cmd(std::move(undo_cmd)), elevated(std::move(elevated)) {}
    explicit prep_cmd_t(std::string &&do_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)), elevated(std::move(elevated)) {}
    std::string do_cmd;
    std::string undo_cmd;
    bool elevated;
  };
  struct sunshine_t {
    std::string locale;
    int min_log_level;
    std::bitset<flag::FLAG_SIZE> flags;
    std::string credentials_file;

    std::string username;
    std::string password;
    std::string salt;

    std::string config_file;

    struct cmd_t {
      std::string name;
      int argc;
      char **argv;
    } cmd;

    std::uint16_t port;
    std::string address_family;

    std::string log_file;

    std::vector<prep_cmd_t> prep_cmds;
  };

  extern video_t video;
  extern audio_t audio;
  extern stream_t stream;
  extern nvhttp_t nvhttp;
  extern input_t input;
  extern sunshine_t sunshine;

  int
  parse(int argc, char *argv[]);
  std::unordered_map<std::string, std::string>
  parse_config(const std::string_view &file_content);
}  // namespace config
