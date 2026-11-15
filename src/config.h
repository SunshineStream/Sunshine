/**
 * @file src/config.h
 * @brief Declarations for the configuration of Sunshine.
 */
#pragma once

// standard includes
#include <bitset>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// local includes
#include "nvenc/nvenc_config.h"
#include "string_hash.h"

namespace config {
  /**
   * @brief Owning configuration entries supporting lookup by string view or C string.
   */
  using config_map_t = std::unordered_map<std::string, std::string, util::transparent_string_hash, std::equal_to<>>;

  namespace nv {
    /**
     * @brief Convert Sunshine's NVENC quality preset number to FFmpeg's stable symbolic name.
     *
     * @param quality_preset Validated Sunshine quality preset in the range 1 through 7.
     * @return FFmpeg preset name from `p1` through `p7`.
     */
    std::string ffmpeg_preset_from_quality(int quality_preset);
  }  // namespace nv

  // Valid range for the packetsize limit
  constexpr int PACKETSIZE_MIN = 200;  ///< Lowest accepted configured packet size in bytes.
  constexpr int PACKETSIZE_MAX = 65535;  ///< Highest accepted configured packet size in bytes.
  constexpr int PACKETSIZE_SMALL = 500;  ///< Conservative packet size used for low-MTU links.
  constexpr int PACKETSIZE_LARGE = 1456;  ///< Default large packet size that avoids common MTU fragmentation.

  inline constexpr std::string_view GAMEPAD_DRIVER_ALL = "all";  ///< Allow every available Windows virtual gamepad driver.
  inline constexpr std::string_view GAMEPAD_DRIVER_VIRTUALHID = "virtualhid";  ///< Use Virtual HID Broker for gamepads on Windows or macOS.
  inline constexpr std::string_view GAMEPAD_DRIVER_VIGEMBUS = "vigembus";  ///< Allow only ViGEmBus for Windows gamepads.
  inline constexpr std::string_view GAMEPAD_DRIVER_NONE = "none";  ///< Disable virtual gamepads on Windows and macOS.

  // track modified config options
  inline config_map_t modified_config_settings;  ///< Configuration keys changed during the current parse or UI update.

  // sensitive values that should be redacted from logging
  /**
   * @brief Configuration keys whose values must be hidden in logs.
   */
  inline constexpr std::array redacted_config = {
    "csrf_allowed_origins"
  };

  /**
   * @brief Log configuration entries and optionally mark them for persistence.
   *
   * @param vars Parsed configuration entries to log.
   * @param save Whether modified configuration values should be written back to disk.
   */
  void log_config_settings(const config_map_t &vars, bool save);

  /**
   * @brief Video encoder, capture, and color settings loaded from configuration.
   */
  struct video_t {
    // ffmpeg params
    /**
     * @brief Quantization parameter used by encoders where higher values trade quality for compression.
     */
    int qp;  // higher == more compression and less quality

    int hevc_mode;  ///< HEVC support mode advertised to clients.
    int av1_mode;  ///< AV1 support mode advertised to clients.

    int min_threads;  ///< Minimum number of threads or slices for CPU encoding.

    int max_stream_width;  ///< Maximum streamed video width; larger client requests are downscaled. 0 disables the cap.
    int max_stream_height;  ///< Maximum streamed video height; larger client requests are downscaled. 0 disables the cap.

    struct {
      std::string sw_preset;  ///< FFmpeg software-encoder preset.
      std::string sw_tune;  ///< FFmpeg software-encoder tuning profile.
      std::optional<int> svtav1_preset;  ///< SVT-AV1 preset; unset uses the encoder default.
    } sw;  ///< Software encoder options.

    nvenc::nvenc_config nv;  ///< NVIDIA NVENC encoder settings.
    bool nv_realtime_hags;  ///< Enable the NVIDIA realtime HAGS workaround.
    bool nv_opengl_vulkan_on_dxgi;  ///< Prefer NVIDIA OpenGL/Vulkan-on-DXGI interop.
    bool nv_sunshine_high_power_mode;  ///< Request NVIDIA high-power mode for Sunshine.

    struct {
      std::string preset;  ///< FFmpeg NVENC preset name shared by supported FFmpeg versions.
      int multipass;  ///< Legacy NVENC multipass mode.
      int h264_coder;  ///< Legacy NVENC H.264 entropy-coding mode.
      int spatial_aq;  ///< FFmpeg NVENC spatial adaptive-quantization mode.
      int vbv_percentage_increase;  ///< Legacy NVENC VBV buffer-size percentage increase.
    } nv_legacy;  ///< Legacy NVIDIA encoder options kept for config compatibility.

    struct {
      std::optional<int> qsv_preset;  ///< Intel Quick Sync preset; unset uses the encoder default.
      std::optional<int> qsv_cavlc;  ///< Intel Quick Sync CAVLC selection; unset uses the encoder default.
      bool qsv_slow_hevc;  ///< Whether to enable the slower Intel Quick Sync HEVC path.
    } qsv;  ///< Intel Quick Sync encoder options.

    struct {
      std::optional<int> amd_usage_h264;  ///< AMF H.264 usage profile; unset uses the encoder default.
      std::optional<int> amd_usage_hevc;  ///< AMF HEVC usage profile; unset uses the encoder default.
      std::optional<int> amd_usage_av1;  ///< AMF AV1 usage profile; unset uses the encoder default.
      std::optional<int> amd_rc_h264;  ///< AMF H.264 rate-control mode; unset uses the encoder default.
      std::optional<int> amd_rc_hevc;  ///< AMF HEVC rate-control mode; unset uses the encoder default.
      std::optional<int> amd_rc_av1;  ///< AMF AV1 rate-control mode; unset uses the encoder default.
      std::optional<int> amd_enforce_hrd;  ///< AMF HRD enforcement setting; unset uses the encoder default.
      std::optional<int> amd_quality_h264;  ///< AMF H.264 quality preset; unset uses the encoder default.
      std::optional<int> amd_quality_hevc;  ///< AMF HEVC quality preset; unset uses the encoder default.
      std::optional<int> amd_quality_av1;  ///< AMF AV1 quality preset; unset uses the encoder default.
      std::optional<int> amd_preanalysis;  ///< AMF pre-analysis setting; unset uses the encoder default.
      std::optional<int> amd_vbaq;  ///< AMF variance-based adaptive-quantization setting; unset uses the encoder default.
      std::optional<int> amd_max_au_size;  ///< Maximum AMF H.264/HEVC access unit size in bits; unset uses the encoder default.
      int amd_coder;  ///< AMF entropy-coding mode.
    } amd;  ///< AMD AMF encoder options.

    struct {
      int vt_realtime;  ///< Whether VideoToolbox uses realtime encoding mode.
      int vt_coder;  ///< VideoToolbox entropy-coding mode.
    } vt;  ///< VideoToolbox encoder options.

    struct {
      std::optional<int> blbrc;  ///< VA-API block-level bitrate-control setting; unset uses the encoder default.
      std::optional<int> vaapi_quality;  ///< VA-API quality setting; unset uses the encoder default.
      std::optional<int> vaapi_rc;  ///< VA-API rate-control mode; unset uses the encoder default.
      std::string vaapi_rc_str;  ///< Text representation of the selected VA-API rate-control mode.
      bool strict_rc_buffer;  ///< Whether VA-API must strictly enforce the rate-control buffer.
    } vaapi;  ///< VA-API encoder options.

    struct {
      int tune;  ///< Vulkan encoder tuning mode: default, HQ, LL, ULL, or lossless.
      int rc_mode;  ///< Vulkan encoder rate-control mode: driver, CQP, CBR, or VBR.
      int quality;  ///< Vulkan encoder quality preset: 1 = speed, 2 = balanced (default), 3 = quality.
    } vk;  ///< Vulkan encoder options.

    std::string capture;  ///< Capture backend name selected by configuration.
    bool fp16_sdr_gamma_encoded;  ///< Treat FP16 captures of SDR displays as already sRGB gamma-encoded (Windows).
    std::string encoder;  ///< Encoder backend name selected by configuration.
    std::string adapter_name;  ///< Display adapter name selected in configuration.
    std::string output_name;  ///< Display output name selected in configuration.

    /**
     * @brief Display-device integration settings.
     */
    struct dd_t {
      /**
       * @brief Compatibility workarounds for display-device control.
       */
      struct workarounds_t {
        std::chrono::milliseconds hdr_toggle_delay;  ///< Specify whether to apply HDR high-contrast color workaround and what delay to use.
      };

      /**
       * @brief Selects how Sunshine prepares the active display before streaming.
       */
      enum class config_option_e {
        disabled,  ///< Disable the configuration for the device.
        verify_only,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_active,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_primary,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_only_display  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
      };

      /**
       * @brief Selects how Sunshine chooses the stream display resolution.
       */
      enum class resolution_option_e {
        disabled,  ///< Do not change resolution.
        automatic,  ///< Change resolution and use the one received from Moonlight.
        manual  ///< Change resolution and use the manually provided one.
      };

      /**
       * @brief Selects how Sunshine chooses the stream display refresh rate.
       */
      enum class refresh_rate_option_e {
        disabled,  ///< Do not change refresh rate.
        automatic,  ///< Change refresh rate and use the one received from Moonlight.
        manual  ///< Change refresh rate and use the manually provided one.
      };

      /**
       * @brief Selects how Sunshine handles HDR state for the stream display.
       */
      enum class hdr_option_e {
        disabled,  ///< Do not change HDR settings.
        automatic  ///< Change HDR settings and use the state requested by Moonlight.
      };

      /**
       * @brief Single display mode remapping rule from configuration.
       */
      struct mode_remapping_entry_t {
        std::string requested_resolution;  ///< Resolution string requested by the client.
        std::string requested_fps;  ///< Refresh-rate string requested by the client.
        std::string final_resolution;  ///< Resolution string to apply after remapping.
        std::string final_refresh_rate;  ///< Refresh-rate string to apply after remapping.
      };

      /**
       * @brief Collection of display mode remapping rules.
       */
      struct mode_remapping_t {
        std::vector<mode_remapping_entry_t> mixed;  ///< To be used when `resolution_option` and `refresh_rate_option` is set to `automatic`.
        std::vector<mode_remapping_entry_t> resolution_only;  ///< To be use when only `resolution_option` is set to `automatic`.
        std::vector<mode_remapping_entry_t> refresh_rate_only;  ///< To be use when only `refresh_rate_option` is set to `automatic`.
      };

      config_option_e configuration_option;  ///< Display-preparation mode selected by configuration.
      resolution_option_e resolution_option;  ///< Resolution-selection mode selected by configuration.
      std::string manual_resolution;  ///< Manual resolution in case `resolution_option == resolution_option_e::manual`.
      refresh_rate_option_e refresh_rate_option;  ///< Refresh-rate selection mode selected by configuration.
      std::string manual_refresh_rate;  ///< Manual refresh rate in case `refresh_rate_option == refresh_rate_option_e::manual`.
      hdr_option_e hdr_option;  ///< HDR-selection mode selected by configuration.
      std::chrono::milliseconds config_revert_delay;  ///< Time to wait until settings are reverted (after stream ends/app exists).
      bool config_revert_on_disconnect;  ///< Specify whether to revert display configuration on client disconnect.
      mode_remapping_t mode_remapping;  ///< Display mode remapping rules grouped by automatic selection mode.
      workarounds_t wa;  ///< Display-device compatibility workarounds.
    };

    dd_t dd;  ///< Display-device integration settings.

    int max_bitrate;  ///< Maximum bitrate ceiling in kbps for bitrate requested from the client.
    double minimum_fps_target;  ///< Lowest framerate that will be used when streaming. Range 0-1000, 0 = half of client's requested framerate.
  };

  /**
   * @brief Audio capture and encoder settings loaded from configuration.
   */
  struct audio_t {
    std::string sink;  ///< Audio output device/sink to use for audio capture
    std::string virtual_sink;  ///< Virtual audio sink for audio routing
    bool stream;  ///< Enable audio streaming to clients
    bool install_steam_drivers;  ///< Install Steam audio drivers for enhanced compatibility
    bool external_audio;  ///< Windows capture-only mode; an external router owns endpoint defaults and formats.
  };

  /**
   * @brief Encryption policy that always sends unencrypted video.
   */
  constexpr int ENCRYPTION_MODE_NEVER = 0;  // Never use video encryption, even if the client supports it
  /**
   * @brief Encryption policy that uses encrypted video only when the client supports it.
   */
  constexpr int ENCRYPTION_MODE_OPPORTUNISTIC = 1;  // Use video encryption if available, but stream without it if not supported
  /**
   * @brief Encryption policy that rejects clients without video encryption support.
   */
  constexpr int ENCRYPTION_MODE_MANDATORY = 2;  // Always use video encryption and refuse clients that can't encrypt

  /**
   * @brief Network stream settings shared by audio, video, and control channels.
   */
  struct stream_t {
    std::chrono::milliseconds ping_timeout;  ///< Timeout used when waiting for client ping responses.

    std::string file_apps;  ///< Path to the configured applications file.

    int fec_percentage;  ///< Percentage of forward-error-correction packets to add to the stream (0-100); 0 disables FEC.

    // Video encryption settings for LAN and WAN streams
    int lan_encryption_mode;  ///< Video encryption policy for LAN clients.
    int wan_encryption_mode;  ///< Video encryption policy for WAN clients.

    // Limit the packetsize to avoid fragmentation on a low MTU link
    int packetsize;  ///< Maximum payload size for network packets.

    bool dscp_tagging;  ///< Allow DSCP tagging of audio and video traffic when requested by the client.
  };

  /**
   * @brief HTTP and HTTPS settings used by the GameStream pairing server.
   */
  struct nvhttp_t {
    // Could be any of the following values:
    // pc|lan|wan
    std::string origin_web_ui_allowed;  ///< Origin policy used for Web UI access checks.

    std::string pkey;  ///< Private key PEM string or path.
    std::string cert;  ///< Certificate PEM string or path.

    std::string sunshine_name;  ///< Host name advertised to Moonlight clients.

    std::string file_state;  ///< Path to the persisted Sunshine state file.

    std::string external_ip;  ///< External address advertised to clients when configured.
  };

  /**
   * @brief Input emulation settings loaded from configuration.
   */
  struct input_t {
    std::unordered_map<int, int> keybindings;  ///< Client keycode to platform keycode bindings.

    std::chrono::milliseconds back_button_timeout;  ///< Hold duration that turns a controller Back button into a special action.
    std::chrono::milliseconds key_repeat_delay;  ///< Delay before repeating a held keyboard key.
    std::chrono::duration<double> key_repeat_period;  ///< Interval between repeated keyboard key events.

    std::string gamepad;  ///< Virtual controller profile selected by configuration.
    std::string gamepad_driver;  ///< Virtual gamepad backend policy on Windows and macOS.
    bool ds4_back_as_touchpad_click;  ///< Map Back/Select to touchpad click for PlayStation-style gamepads.
    bool motion_as_ds4;  ///< Prefer PlayStation-style emulation for client gamepads with motion controls.
    bool touchpad_as_ds4;  ///< Prefer PlayStation-style emulation for client gamepads with touchpad input.
    bool virtualhid_randomize_mac;  ///< Randomize the libvirtualhid virtual controller MAC address.

    bool keyboard;  ///< Enable keyboard input from clients.
    bool key_rightalt_to_key_win;  ///< Map the client Right Alt key to the Windows key.
    bool mouse;  ///< Enable mouse input from clients.
    bool controller;  ///< Enable controller input from clients.

    bool always_send_scancodes;  ///< Always send keyboard scancodes when available.

    bool high_resolution_scrolling;  ///< Enable high-resolution mouse-wheel events.
    bool native_pen_touch;  ///< Enable native pen and touch injection.
  };

  namespace flag {
    /**
     * @brief Enumerates supported flag options.
     */
    enum flag_e : std::size_t {
      PIN_STDIN = 0,  ///< Read PIN from stdin instead of http
      FRESH_STATE,  ///< Do not load or save state
      FORCE_VIDEO_HEADER_REPLACE,  ///< force replacing headers inside video data
      UPNP,  ///< Try Universal Plug 'n Play
      CONST_PIN,  ///< Use "universal" pin
      FLAG_SIZE  ///< Number of flags
    };
  }  // namespace flag

  /**
   * @brief External preparation command plus its privilege requirement.
   */
  struct prep_cmd_t {
    /**
     * @brief Build a preparation command entry from parsed configuration data.
     *
     * @param do_cmd Command to run before the application starts.
     * @param undo_cmd Command to run after the application exits.
     * @param elevated Whether the command should run with elevated privileges.
     */
    prep_cmd_t(std::string &&do_cmd, std::string &&undo_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)),
        undo_cmd(std::move(undo_cmd)),
        elevated(elevated) {
    }

    /**
     * @brief Build a preparation command entry from parsed configuration data.
     *
     * @param do_cmd Command to run before the application starts.
     * @param elevated Whether the command should run with elevated privileges.
     */
    explicit prep_cmd_t(std::string &&do_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)),
        elevated(elevated) {
    }

    std::string do_cmd;  ///< Command to run before the application starts.
    std::string undo_cmd;  ///< Command to run after the application exits.
    bool elevated;  ///< Whether the process should be launched elevated.
  };

  /**
   * @brief Top-level Sunshine configuration and credential state.
   */
  struct sunshine_t {
    std::string locale;  ///< Locale selected for Sunshine UI and log messages.
    int min_log_level;  ///< Minimum severity level written to the configured log sink.
    std::bitset<flag::FLAG_SIZE> flags;  ///< Runtime flags parsed from command-line options.
    std::string credentials_file;  ///< Path to the stored pairing credentials file.

    std::string username;  ///< Username for the local Web UI account.
    std::string password;  ///< Password hash or secret for the local Web UI account.
    std::string salt;  ///< Salt used when hashing the Web UI password.

    std::string config_file;  ///< Path to the active Sunshine configuration file.

    /**
     * @brief Command-line options parsed before configuration loading.
     */
    struct cmd_t {
      std::string name;  ///< Executable name from the command line.
      int argc;  ///< Number of command-line arguments.
      char **argv;  ///< Command-line argument vector.
    };

    cmd_t cmd;  ///< Command line used to launch the application.

    std::uint16_t port;  ///< TCP port used by Sunshine services.
    std::string address_family;  ///< Address family requested for listening sockets.
    std::string bind_address;  ///< Local address Sunshine should bind to.

    std::string log_file;  ///< Path to the configured log file.
    bool notify_pre_releases;  ///< Notify users about pre-release updates.
    bool system_tray;  ///< Enable the system tray integration.
    std::vector<prep_cmd_t> prep_cmds;  ///< Preparation commands executed around application launch.

    // List of allowed origins for CSRF protection (e.g., "https://example.com,https://app.example.com")
    // Comma-separated list of additional origins. Default includes localhost variants and web UI port.
    std::vector<std::string> csrf_allowed_origins;  ///< Additional origins allowed by CSRF validation.
  };

  extern video_t video;
  extern audio_t audio;
  extern stream_t stream;
  extern nvhttp_t nvhttp;
  extern input_t input;
  extern sunshine_t sunshine;

#ifdef SUNSHINE_TESTS
  /**
   * @brief Parse and apply serialized configuration text for unit tests.
   *
   * @param file_content Raw configuration text to parse and apply.
   */
  void apply_config_for_test(std::string_view file_content);
#endif

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param argc Number of command-line arguments.
   * @param argv Command-line argument vector.
   * @return 0 on success; nonzero when command-line or configuration parsing fails.
   */
  int parse(int argc, char *argv[]);
  /**
   * @brief Parse Sunshine configuration text into key-value entries.
   *
   * @param file_content Raw configuration file contents to parse.
   * @return Parsed configuration key-value entries.
   */
  config_map_t parse_config(const std::string_view &file_content);

  /**
   * @brief Persist a configuration option when the active file does not already define it.
   *
   * Existing text and comments are retained because the new setting is appended rather
   * than serializing the complete configuration again.
   *
   * @param name Configuration option name.
   * @param value Configuration option value.
   * @return True when the option was appended successfully; otherwise, false.
   */
  bool persist_config_option_if_missing(std::string_view name, std::string_view value);

  /**
   * @brief Select all available gamepad drivers when a licensed user has not made a choice.
   *
   * @param virtualhid_licensed Whether Virtual HID Driver has an active machine license.
   * @return True when the `all` preference was persisted and applied; otherwise, false.
   */
  bool select_all_gamepad_drivers_if_licensed(bool virtualhid_licensed);

  /**
   * @brief Apply the configured stream resolution cap to a client-requested resolution.
   *
   * The result preserves the aspect ratio of the request and is rounded down to even
   * dimensions as required by encoders and decoders.
   * @param width Client-requested width, updated in place when the cap applies.
   * @param height Client-requested height, updated in place when the cap applies.
   * @return `true` when the resolution was capped.
   */
  bool cap_stream_resolution(int &width, int &height);
}  // namespace config
