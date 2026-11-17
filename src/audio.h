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
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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
   * selection (virtual sink when host playback is muted and no sink is
   * configured) together with the swap-vs-capture policy. The capture-only path
   * targets the configured sink, or the saved host sink when nothing is
   * configured: without a swap, applications stay on the host default, so
   * capturing the virtual sink's monitor would yield silence.
   *
   * The seven positional parameters are an accepted deviation from the bundle
   * rule: one call site, one pure function, and bundling would only rename the
   * same seven inputs.
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
    std::string_view host_sink,
    std::string_view config_sink,
    const std::optional<platf::sink_t::null_t> &null_sinks
  ) {
    // Order of priority: explicit audio sink, virtual sink, host.
    std::string selected {host_sink};
    if (!config_sink.empty()) {
      selected = std::string {config_sink};
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink.
    // An explicit audio_sink pin always wins: the user chose that target on purpose.
    if (config_sink.empty() && null_sinks && (!host_audio_enabled || selected.empty())) {
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
        default:
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
      routing.target = config_sink.empty() ? std::string {host_sink} : std::string {config_sink};
    }

    return routing;
  }

  /**
   * @brief Resolve the user-level capture intent from the audio configuration.
   *
   * @param keep_default_requested Value of config::audio.keep_default_sink.
   * @param config_sink Value of config::audio.sink.
   * @return Pinned intent when a sink is configured, host-and-client for
   *         keep-default (Play on this PC too), and client-only otherwise
   *         (Quiet streaming).
   */
  [[nodiscard]] constexpr platf::audio_capture_intent_e resolve_audio_capture_intent(bool keep_default_requested, std::string_view config_sink) noexcept {
    using enum platf::audio_capture_intent_e;
    if (!config_sink.empty()) {
      return pinned_sink;
    }
    return keep_default_requested ? host_and_client : client_only;
  }

  /**
   * @brief Stable name of a capture intent, for logging.
   *
   * @param intent Capture intent to name.
   * @return Intent name without the enum prefix.
   */
  [[nodiscard]] constexpr std::string_view audio_capture_intent_name(platf::audio_capture_intent_e intent) noexcept {
    using enum platf::audio_capture_intent_e;
    switch (intent) {
      case client_only:
        return "client_only";
      case host_and_client:
        return "host_and_client";
      case pinned_sink:
        return "pinned_sink";
    }
    return "unknown";
  }

  /**
   * @brief Whether the launched process tree should be captured directly.
   *
   * Silence takes priority for client-only intent: per-process capture is
   * only selected for host-and-client intent, where capturing the game
   * process must not defeat a silent route. A pinned sink always wins,
   * because a configured sink resolves to pinned intent.
   *
   * @param intent User-level capture intent.
   * @param process_capture_supported Whether the backend captures a process tree.
   * @param process_id Launched application PID, or 0 when unavailable.
   * @return True when the backend should capture the process tree.
   */
  [[nodiscard]] constexpr bool should_capture_process_tree(platf::audio_capture_intent_e intent, bool process_capture_supported, std::uint32_t process_id) noexcept {
    using enum platf::audio_capture_intent_e;
    return intent == host_and_client && process_capture_supported && process_id != 0;
  }

  /**
   * @brief Select the capture sink for startup without touching the capture target.
   *
   * Encodes the swap-vs-capture dispatch used by capture(): the swap path swaps
   * the host default via set_sink(), the capture-only path captures via
   * capture_sink(). A swap with nothing to restore skips the sink call entirely
   * because the target is already the host sink.
   *
   * @param control Platform audio controller used for sink selection.
   * @param routing Startup plan resolved by resolve_capture_routing().
   * @return True when capture may proceed; false when sink selection failed and the caller must abort.
   */
  bool select_audio_capture_sink(platf::audio_control_t &control, const capture_routing_t &routing);

  /**
   * @brief RAII owner of one session's shared capture target.
   *
   * Sessions share one audio control. Every create_microphone() call refreshes
   * the target (set_capture_target()) and creates the microphone under one
   * lock acquisition, so a refresh can never be dropped or interleaved with
   * another session's refresh. Destruction releases exactly the last recorded
   * target via clear_capture_target(), or nothing when no refresh ever ran
   * (for example after an aborted sink selection), so one session cannot wipe
   * another session's recorded target on its way out.
   *
   * The referenced control and mutex must outlive the guard.
   */
  class session_capture_target_t {
  public:
    /**
     * @brief Create a guard that owns no target until the first refresh.
     *
     * @param control Platform audio controller holding the shared capture target.
     * @param intent Capture intent resolved from the audio configuration.
     * @param mutex Serializes refresh, microphone creation, and target release across sessions.
     */
    session_capture_target_t(platf::audio_control_t &control, platf::audio_capture_intent_e intent, std::mutex &mutex);

    /**
     * @brief Destroy the guard, releasing the recorded target when one exists.
     */
    ~session_capture_target_t();

    session_capture_target_t(const session_capture_target_t &) = delete;
    session_capture_target_t &operator=(const session_capture_target_t &) = delete;

    /**
     * @brief Refresh the capture target, then create the microphone for it.
     *
     * The refresh always runs before microphone creation so a dropped refresh
     * is observable: set_capture_target() is called on every invocation,
     * including reinitializations and microphone failures. The lock covers
     * only the refresh and the post-creation re-check, never microphone
     * creation itself; when the recorded target changed during creation the
     * microphone is discarded and creation retries once.
     *
     * @param process_capture_supported Whether the backend captures a process tree.
     * @param capture_process_identity_provider Optional callable returning the launched application identity ({0, 0} for endpoint capture).
     * @param mic_params Microphone capture parameters for the requested layout.
     * @return Microphone capture object, or nullptr when creation failed.
     */
    [[nodiscard]] std::unique_ptr<platf::mic_t> create_microphone(
      bool process_capture_supported,
      const std::function<platf::process_identity_t()> &capture_process_identity_provider,
      const platf::microphone_params_t &mic_params
    );

    /**
     * @brief Get the capture intent owned by this session.
     * @return Capture intent passed to the constructor.
     */
    [[nodiscard]] platf::audio_capture_intent_e intent() const noexcept {
      return intent_;
    }

    /**
     * @brief Get the PID from the last refresh, or 0 when no refresh ran.
     * @return Effective PID recorded via set_capture_target().
     */
    [[nodiscard]] std::uint32_t process_id() const noexcept {
      return process_id_;
    }

    /**
     * @brief Check whether this session recorded a target that teardown will release.
     * @return True after the first create_microphone() call.
     */
    [[nodiscard]] bool recorded() const noexcept {
      return recorded_;
    }

  private:
    platf::audio_control_t *control_;  ///< Non-owning controller holding the shared capture target; must outlive the guard.
    platf::audio_capture_intent_e intent_;  ///< Capture intent recorded by this session.
    std::mutex *mutex_;  ///< Non-owning mutex serializing refresh, creation, and release; typically audio::capture_target_mutex().
    std::uint32_t process_id_ = 0;  ///< PID from the last refresh, or 0 for endpoint capture.
    bool recorded_ = false;  ///< Whether this session called set_capture_target().
  };

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
   * @param capture_process_identity_provider Optional callable returning the launched application identity ({0, 0} for endpoint capture).
   */
  void capture(safe::mail_t mail, config_t config, stream::session_t *channel_data, std::function<platf::process_identity_t()> capture_process_identity_provider = {});

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
