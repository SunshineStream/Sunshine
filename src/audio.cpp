/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <array>
#include <mutex>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  /**
   * @brief Owning pointer for an Opus multistream encoder.
   */
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  /**
   * @brief Shared queue carrying captured PCM sample buffers to the encoder thread.
   */
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  /**
   * @brief Serializes capture-target refresh, microphone creation, and target release across concurrent sessions.
   *
   * All capture() threads share one audio control. Each session's
   * session_capture_target_t guard holds this mutex across the refresh
   * and the exit clear, but never across microphone creation itself,
   * which can wait on endpoint activation; holding the lock across it
   * would stall every other session's teardown.
   *
   * Lock order: this outer mutex is always taken before the backend-internal
   * `platf::audio_control_t::capture_state_mutex_`, acquired inside
   * `set_capture_target()`/`microphone()`/`clear_capture_target()`. Backends
   * never take this mutex, so the order cannot invert.
   *
   * @return Reference to the process-lifetime capture-target mutex.
   */
  std::mutex &capture_target_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  /**
   * @brief Select the Opus stream configuration for a channel count and quality tier.
   *
   * @param channels Number of audio channels in the stream.
   * @param quality Whether the high-quality Opus layout should be selected.
   * @return Index into `stream_configs` for the requested layout.
   */
  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;  ///< Audio sample rate in hertz required by Opus.

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  /**
   * @brief Opus stream layouts and bitrates advertised to clients.
   */
  const std::array<opus_stream_config_t, MAX_STREAM_CONFIG> stream_configs {{
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo.data(),
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo.data(),
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51.data(),
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51.data(),
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71.data(),
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71.data(),
      2048000,
    },
  }};

  /**
   * @brief Encode captured PCM samples into Opus packets on the audio worker thread.
   *
   * @param samples Queue of captured PCM sample buffers to encode.
   * @param config Audio stream settings negotiated with the client.
   * @param channel_data Stream session carried with each encoded packet.
   */
  void encodeThread(sample_queue_t samples, config_t config, stream::session_t *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  /**
   * @brief Select the capture sink for startup without touching the capture target.
   */
  bool select_audio_capture_sink(platf::audio_control_t &control, const capture_routing_t &routing) {
    if (routing.swap_at_startup) {
      // The target is already the host sink when there is nothing to restore,
      // so the swap is skipped entirely instead of reasserting the default.
      if (routing.restore_at_teardown && control.set_sink(routing.target)) {
        return false;
      }
    } else if (control.capture_sink(routing.target)) {
      return false;
    }
    return true;
  }

  /**
   * @brief Create a guard that owns no target until the first refresh.
   */
  session_capture_target_t::session_capture_target_t(platf::audio_control_t &control, platf::audio_capture_intent_e intent, std::mutex &mutex):
      control_ {&control},
      intent_ {intent},
      mutex_ {&mutex} {
  }

  /**
   * @brief Destroy the guard, releasing the recorded target when one exists.
   */
  session_capture_target_t::~session_capture_target_t() {
    if (!recorded_) {
      return;
    }
    std::lock_guard lock {*mutex_};
    control_->clear_capture_target(intent_, 0);
  }

  /**
   * @brief Refresh the endpoint capture target, then create the microphone for it.
   */
  std::unique_ptr<platf::mic_t> session_capture_target_t::create_microphone(const platf::microphone_params_t &mic_params) {
    // Refresh the endpoint target before every microphone creation. Only the
    // refresh runs under the lock: microphone creation can wait on endpoint
    // activation, and holding the lock across it would stall every other
    // session's teardown. The refresh is recorded even when microphone
    // creation fails so teardown still releases this session's target.
    {
      std::lock_guard lock {*mutex_};
      control_->set_capture_target(intent_, 0);
      recorded_ = true;
    }
    return control_->microphone(mic_params);
  }

  /**
   * @brief Run the capture loop for this backend.
   *
   * Sessions share one audio control: sink selection runs through
   * select_audio_capture_sink(), the restore flag is assigned directly from
   * the resolved routing, and one session_capture_target_t guard owns the
   * shared target for the session. Both microphone call sites (startup and
   * reinitialization) are the same single guard call, and guard destruction
   * releases the shared target only when a refresh ran.
   */
  void capture(safe::mail_t mail, config_t config, stream::session_t *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    const auto capture_intent = resolve_audio_capture_intent(config::audio.keep_default_sink, config::audio.sink);

    // One RAII owner for the session's shared capture target. Destruction
    // releases exactly the last recorded target, or nothing when no refresh
    // ran, so one session cannot wipe another session's recorded target.
    session_capture_target_t capture_target {*control, capture_intent, capture_target_mutex()};

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // One decision point for swap-vs-capture: the recorded restore flag keeps
      // teardown symmetric with startup on every platform, including backends
      // that never learned the keep-default behavior.
      const auto routing = resolve_capture_routing(
        config::audio.keep_default_sink,
        control->supports_keep_default_sink(),
        config.flags[config_t::HOST_AUDIO],
        stream.channelCount,
        ref->sink.host,
        config::audio.sink,
        ref->sink.null
      );
      ref->restore_sink = routing.restore_at_teardown;
      BOOST_LOG(info) << "Audio capture mode ["sv << (routing.swap_at_startup ? "swap-and-restore"sv : "capture-keep-default"sv)
                      << "], target ["sv << routing.target
                      << "], restore_at_teardown ["sv << routing.restore_at_teardown
                      << "], keep_default_sink requested ["sv << config::audio.keep_default_sink
                      << "], keep_default_sink supported ["sv << control->supports_keep_default_sink()
                      << "], capture intent ["sv << audio_capture_intent_name(capture_intent) << ']';
      // If the selected sink cannot be selected, there is no audio to capture.
      if (!select_audio_capture_sink(*control, routing)) {
        return;
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    const platf::microphone_params_t mic_params {
      stream.mapping,
      stream.channelCount,
      static_cast<std::uint32_t>(stream.sampleRate),
      static_cast<std::uint32_t>(frame_size),
      continuous_audio,
      host_audio
    };
    auto mic = capture_target.create_microphone(mic_params);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::jthread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&samples, &thread, &shutdown_event]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = capture_target.create_microphone(mic_params);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    // Keep-default captures the configured sink, so probe that endpoint rather
    // than the host default recorded at startup.
    if (config::audio.keep_default_sink && !config::audio.sink.empty() && ctx.control->supports_keep_default_sink()) {
      return ctx.control->is_sink_available(config::audio.sink);
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  /**
   * @brief Select the Opus stream configuration for a channel count and quality tier.
   */
  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable. Startup recorded whether it swapped;
    // keep-default sessions never swap, so there is nothing to restore.
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping.data();
  }
}  // namespace audio
