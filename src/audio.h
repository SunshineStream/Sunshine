/**
 * @file src/audio.h
 * @brief Declarations for audio capture and encoding.
 */
#pragma once

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

#include <array>
#include <bitset>
#include <optional>
#include <string>

namespace stream {
  struct session_t;
}

namespace audio {
  /**
   * @brief Supported Opus channel layouts advertised to Moonlight clients.
   */
  enum stream_config_e : int {
    STEREO,  ///< Stereo
    HIGH_STEREO,  ///< High stereo
    SURROUND51,  ///< Surround 5.1
    HIGH_SURROUND51,  ///< High surround 5.1
    SURROUND71,  ///< Surround 7.1
    HIGH_SURROUND71,  ///< High surround 7.1
    MAX_STREAM_CONFIG  ///< Maximum audio stream configuration
  };

  /**
   * @brief Static Opus encoder layout for one advertised audio mode.
   */
  struct opus_stream_config_t {
    std::int32_t sampleRate;  ///< Opus sample rate in hertz.
    int channelCount;  ///< Number of audio channels in the Opus layout.
    int streams;  ///< Number of Opus streams in the layout.
    int coupledStreams;  ///< Number of stereo-coupled Opus streams.
    const std::uint8_t *mapping;  ///< Channel mapping table passed to the Opus encoder.
    int bitrate;  ///< Target bitrate in bits per second.
  };

  /**
   * @brief Custom Opus channel layout supplied by configuration.
   */
  struct stream_params_t {
    int channelCount;  ///< Number of audio channels in the Opus layout.
    int streams;  ///< Number of Opus streams in the custom layout.
    int coupledStreams;  ///< Number of stereo-coupled Opus streams.
    std::array<std::uint8_t, 8> mapping;  ///< Channel mapping table for up to eight speakers.
  };

  /**
   * @brief Opus layouts and bitrates advertised for each supported stream mode.
   */
  extern const std::array<opus_stream_config_t, MAX_STREAM_CONFIG> stream_configs;

  /**
   * @brief Audio capture and encoder settings for a stream.
   */
  struct config_t {
    /**
     * @brief Boolean audio feature flags.
     */
    enum flags_e : int {
      HIGH_QUALITY,  ///< High quality audio
      HOST_AUDIO,  ///< Host audio
      CUSTOM_SURROUND_PARAMS,  ///< Custom surround parameters
      CONTINUOUS_AUDIO,  ///< Continuous audio
      MAX_FLAGS  ///< Maximum number of flags
    };

    int packetDuration;  ///< Packet duration in milliseconds requested by the client.
    int channels;  ///< Number of audio channels requested by the client.
    int mask;  ///< Speaker mask describing the requested channel layout.

    stream_params_t customStreamParams;  ///< Custom Opus layout used when CUSTOM_SURROUND_PARAMS is enabled.

    std::bitset<MAX_FLAGS> flags;  ///< Enabled audio feature flags.
  };

  /**
   * @brief Shared audio device state used while streams are active.
   */
  struct audio_ctx_t {
    // We want to change the sink for the first stream only
    std::unique_ptr<std::atomic_bool> sink_flag;  ///< Tracks whether the capture sink was already switched.

    std::unique_ptr<platf::audio_control_t> control;  ///< Platform audio-control implementation.

    bool restore_sink;  ///< Whether Sunshine should restore the original sink when capture ends.
    platf::sink_t sink;  ///< Original sink captured before Sunshine switched devices.
  };

  /**
   * @brief Startup/teardown sink behavior resolved for one capture session.
   *
   * Both fields always agree: a session either swaps the host default and
   * restores it, or captures without touching it. Recording one decision at
   * startup keeps teardown symmetric on every platform.
   */
  struct sink_routing_policy_t {
    bool swap_at_startup;  ///< Whether startup may change the host default sink.
    bool restore_at_teardown;  ///< Whether teardown must restore the previous sink.
  };

  /**
   * @brief Resolve the sink routing policy from the user request and backend capability.
   *
   * Pure function: no I/O, no global state. Backends that never learned the
   * keep-default behavior get the historical swap-and-restore path even when
   * the option is enabled, so enabling it can never strand a foreign default.
   *
   * @param keep_default_requested Value of config::audio.keep_default_sink.
   * @param platform_supports_keep_default Value of platf::audio_control_t::supports_keep_default_sink().
   * @return Policy with matching startup and teardown behavior.
   */
  [[nodiscard]] constexpr sink_routing_policy_t resolve_sink_routing_policy(bool keep_default_requested, bool platform_supports_keep_default) noexcept {
    if (keep_default_requested && platform_supports_keep_default) {
      return {false, false};
    }

    return {true, true};
  }

  /**
   * @brief Capture startup plan: which sink to select and whether to swap the host default.
   */
  struct capture_routing_t {
    bool swap_at_startup;  ///< True selects set_sink(); false selects capture_sink().
    bool restore_at_teardown;  ///< Whether teardown must restore the previous sink.
    std::string target;  ///< Sink name to pass to set_sink() or capture_sink().
  };

  /**
   * @brief Resolve the capture startup plan from session inputs.
   *
   * Pure function: no I/O, no global state. Encodes the historical sink
   * selection (virtual sink when host playback is muted or nothing else is
   * selected) together with the swap-vs-capture policy. The capture-only path
   * targets the configured sink, or the saved host sink when nothing is
   * configured: without a swap, applications stay on the host default, so
   * capturing the virtual sink's monitor would yield silence.
   *
   * @param keep_default_requested Value of config::audio.keep_default_sink.
   * @param platform_supports_keep_default Value of platf::audio_control_t::supports_keep_default_sink().
   * @param host_audio_enabled Whether host playback stays enabled (Moonlight HOST_AUDIO flag).
   * @param channel_count Stream channel count selecting the virtual sink layout.
   * @param host_sink Saved host default sink name.
   * @param config_sink Configured audio_sink value, possibly empty.
   * @param null_sinks Virtual sink names reported by the backend, when any.
   * @return Startup plan with matching swap, restore, and target behavior.
   */
  [[nodiscard]] inline capture_routing_t resolve_capture_routing(
    bool keep_default_requested,
    bool platform_supports_keep_default,
    bool host_audio_enabled,
    int channel_count,
    const std::string &host_sink,
    const std::string &config_sink,
    const std::optional<platf::sink_t::null_t> &null_sinks
  ) {
    // Order of priority: virtual sink, audio sink, host.
    std::string selected = host_sink;
    if (!config_sink.empty()) {
      selected = config_sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink.
    if (null_sinks && (!host_audio_enabled || selected.empty())) {
      switch (channel_count) {
        case 2:
          selected = null_sinks->stereo;
          break;
        case 6:
          selected = null_sinks->surround51;
          break;
        case 8:
          selected = null_sinks->surround71;
          break;
      }
    }

    const auto policy = resolve_sink_routing_policy(keep_default_requested, platform_supports_keep_default);
    capture_routing_t routing {};
    routing.swap_at_startup = policy.swap_at_startup;
    if (policy.swap_at_startup) {
      routing.restore_at_teardown = policy.restore_at_teardown && host_sink != selected;
      routing.target = selected;
    } else {
      routing.restore_at_teardown = false;
      routing.target = config_sink.empty() ? host_sink : config_sink;
    }

    return routing;
  }

  /**
   * @brief Byte buffer used for encoded audio packet payloads.
   */
  using buffer_t = util::buffer_t<std::uint8_t>;
  /**
   * @brief Encoded audio packet paired with its owning stream session.
   */
  using packet_t = std::pair<stream::session_t *, buffer_t>;
  /**
   * @brief Shared mailbox reference to the global audio context.
   */
  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t;

  /**
   * @brief Capture, encode, and publish audio packets for a stream.
   *
   * @param mail Mailbox used to publish encoded audio packets.
   * @param config Audio capture and encoder settings.
   * @param channel_data Stream session that receives the encoded packets.
   */
  void capture(safe::mail_t mail, config_t config, stream::session_t *channel_data);

  /**
   * @brief Get the reference to the audio context.
   * @returns A shared pointer reference to audio context.
   * @note Aside from the configuration purposes, it can be used to extend the
   *       audio sink lifetime to capture sink earlier and restore it later.
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * @examples_end
   */
  audio_ctx_ref_t get_audio_ctx_ref();

  /**
   * @brief Check if the audio sink held by audio context is available.
   * @returns True if available (and can probably be restored), false otherwise.
   * @note Useful for delaying the release of audio context shared pointer (which
   *       tries to restore original sink).
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * if (audio.get()) {
   *     return is_audio_ctx_sink_available(*audio.get());
   * }
   * return false;
   * @examples_end
   * @param ctx Native context object used by the operation or callback.
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx);
}  // namespace audio
