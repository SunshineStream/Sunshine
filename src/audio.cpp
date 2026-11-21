/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <algorithm>
#include <array>
#include <thread>

// lib includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}
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
   * @brief Encode one interleaved stereo or 5.1 float PCM frame as an AC-3-family access unit.
   */
  class ffmpeg_audio_encoder_t {
  public:
    ffmpeg_audio_encoder_t() = default;
    ffmpeg_audio_encoder_t(const ffmpeg_audio_encoder_t &) = delete;
    ffmpeg_audio_encoder_t &operator=(const ffmpeg_audio_encoder_t &) = delete;
    ffmpeg_audio_encoder_t(ffmpeg_audio_encoder_t &&) = delete;
    ffmpeg_audio_encoder_t &operator=(ffmpeg_audio_encoder_t &&) = delete;

    ~ffmpeg_audio_encoder_t() {
      if (packet_) {
        av_packet_free(&packet_);
      }
      if (frame_) {
        av_frame_free(&frame_);
      }
      if (context_) {
        avcodec_free_context(&context_);
      }
    }

    /**
     * @brief Initialize the selected FFmpeg encoder for stereo or 5.1 PCM.
     *
     * @param codec_id FFmpeg codec identifier for AC-3 or E-AC-3.
     * @param bit_rate Target encoded bitrate in bits per second.
     * @param channels Number of channels; only 2 and 6 are supported.
     * @return True if the encoder context and frame buffers initialize successfully.
     */
    bool initialize(AVCodecID codec_id, std::int64_t bit_rate, int channels) {
      if (channels != 2 && channels != 6) {
        return false;
      }

      const auto *codec = avcodec_find_encoder(codec_id);
      if (!codec) {
        return false;
      }

      context_ = avcodec_alloc_context3(codec);
      if (!context_) {
        return false;
      }

      context_->bit_rate = bit_rate;
      context_->sample_rate = SAMPLE_RATE;
      context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
      context_->time_base = AVRational {1, SAMPLE_RATE};
      context_->thread_count = 1;
      channels_ = channels;

      if (const auto channel_layout = channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_5POINT1_BACK; av_channel_layout_from_mask(&context_->ch_layout, channel_layout) < 0 || avcodec_open2(context_, codec, nullptr) < 0) {
        return false;
      }

      // AC-3 and E-AC-3 use 1536 samples per frame here (32 ms at 48 kHz).
      if (context_->frame_size != 1536) {
        return false;
      }

      frame_ = av_frame_alloc();
      if (!frame_) {
        return false;
      }

      frame_->format = context_->sample_fmt;
      frame_->sample_rate = context_->sample_rate;
      frame_->nb_samples = context_->frame_size;
      if (av_channel_layout_copy(&frame_->ch_layout, &context_->ch_layout) < 0 || av_frame_get_buffer(frame_, 0) < 0) {
        return false;
      }

      packet_ = av_packet_alloc();
      return packet_ != nullptr;
    }

    /**
     * @brief Encode an interleaved floating-point PCM frame into a Dolby access unit.
     *
     * @param interleaved Interleaved float PCM samples for one complete frame.
     * @param output Destination buffer for the encoded access unit.
     * @return Encoded access-unit size in bytes, or -1 if input validation or encoding fails.
     */
    int encode(const std::vector<float> &interleaved, buffer_t &output) {
      if (!frame_ || interleaved.size() != static_cast<std::size_t>(frame_->nb_samples * channels_)) {
        return -1;
      }

      if (av_frame_make_writable(frame_) < 0) {
        return -1;
      }

      for (int channel = 0; channel < channels_; ++channel) {
        auto *destination = static_cast<float *>(static_cast<void *>(frame_->extended_data[channel]));
        for (int sample = 0; sample < frame_->nb_samples; ++sample) {
          destination[sample] = interleaved[static_cast<std::size_t>(sample * channels_ + channel)];
        }
      }

      frame_->pts = next_pts_;
      next_pts_ += frame_->nb_samples;

      if (avcodec_send_frame(context_, frame_) < 0 || avcodec_receive_packet(context_, packet_) < 0) {
        av_packet_unref(packet_);
        return -1;
      }

      const auto packet_size = packet_->size;
      if (packet_size <= 0 || static_cast<std::size_t>(packet_size) > output.size()) {
        av_packet_unref(packet_);
        return -1;
      }

      std::copy_n(packet_->data, packet_size, std::begin(output));
      av_packet_unref(packet_);
      return packet_size;
    }

  private:
    AVCodecContext *context_ {nullptr};
    AVFrame *frame_ {nullptr};
    AVPacket *packet_ {nullptr};
    std::int64_t next_pts_ {0};
    int channels_ {0};
  };

#ifdef SUNSHINE_TESTS
  /**
   * @brief Encode one Dolby frame without starting the audio capture loop.
   * @param eac3 Whether to use E-AC-3 instead of AC-3.
   * @param high_quality Whether to use the higher bitrate.
   * @param channels Number of channels to encode.
   * @param interleaved Interleaved float PCM samples.
   * @param output Caller-provided output packet buffer.
   * @return Encoded packet size, or -1 when initialization or encoding fails.
   */
  int encode_dolby_frame_for_testing(
    bool eac3,
    bool high_quality,
    int channels,
    const std::vector<float> &interleaved,
    buffer_t &output
  ) {
    const auto bitrate_kbps = dolby_bitrate_kbps(eac3, high_quality, channels);
    if (bitrate_kbps <= 0) {
      return -1;
    }
    ffmpeg_audio_encoder_t encoder;
    const auto codec_id = eac3 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
    if (!encoder.initialize(codec_id, static_cast<std::int64_t>(bitrate_kbps) * 1000, channels)) {
      return -1;
    }
    return encoder.encode(interleaved, output);
  }
#endif

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

    if (config.use_ac3 || config.use_eac3) {
      const auto codec_id = config.use_eac3 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
      const auto codec_name = config.use_eac3 ? "E-AC-3"sv : "AC-3"sv;
      const auto bitrate_kbps = dolby_bitrate_kbps(config.use_eac3, config.high_quality_dolby, config.channels);
      ffmpeg_audio_encoder_t encoder;
      if (bitrate_kbps <= 0 || !encoder.initialize(codec_id, static_cast<std::int64_t>(bitrate_kbps) * 1000, config.channels)) {
        BOOST_LOG(error) << "Couldn't initialize the linked FFmpeg "sv << codec_name << " encoder"sv;
        packets->stop();
        return;
      }

      BOOST_LOG(info) << codec_name << " initialized: "sv << config.channels
                      << " channels, 48 kHz, "sv << bitrate_kbps << " kbit/s"sv;
      while (auto sample = samples->pop()) {
        // A 640 kbit/s AC-3 frame is 2560 bytes at 48 kHz; reserve additional packet capacity.
        buffer_t packet {MAX_DOLBY_PACKET_SIZE};
        const auto bytes = encoder.encode(*sample, packet);
        if (bytes < 0) {
          BOOST_LOG(error) << "Couldn't encode "sv << codec_name << " audio frame"sv;
          packets->stop();
          return;
        }

        packet.fake_resize(bytes);
        packets->raise(channel_data, std::move(packet));
      }
      return;
    }

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
   * @brief Run the capture loop for this backend.
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

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
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
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
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

  /**
   * @brief Check whether the linked FFmpeg build provides the AC-3 encoder.
   * @return True when the AC-3 encoder is available in this build.
   */
  bool ac3_available() {
    return avcodec_find_encoder(AV_CODEC_ID_AC3) != nullptr;
  }

  /**
   * @brief Check whether the linked FFmpeg build provides the E-AC-3 encoder.
   * @return True when the E-AC-3 encoder is available in this build.
   */
  bool eac3_available() {
    return avcodec_find_encoder(AV_CODEC_ID_EAC3) != nullptr;
  }

  int dolby_bitrate_kbps(bool eac3, bool high_quality, int channels) {
    if (channels == 2) {
      return high_quality ? 192 : 128;
    }
    if (channels == 6) {
      if (eac3) {
        return high_quality ? 384 : 192;
      }
      return high_quality ? 640 : 384;
    }
    return 0;
  }

  dolby_codec_e configure_dolby_audio(
    config_t &config,
    std::string_view offered,
    std::string_view accepted,
    bool ac3_encoder_available,
    bool eac3_encoder_available,
    bool high_quality
  ) {
    using enum audio::dolby_codec_e;

    config.use_ac3 = false;
    config.use_eac3 = false;
    config.high_quality_dolby = false;
    const bool supported_channel_count = config.channels == 2 || config.channels == 6;
    auto selected_codec = opus;
    if (supported_channel_count && offered == "AC3" && accepted == offered && ac3_encoder_available) {
      selected_codec = ac3;
    } else if (supported_channel_count && offered == "EAC3" && accepted == offered && eac3_encoder_available) {
      selected_codec = eac3;
    }
    if (selected_codec != opus) {
      config.use_ac3 = selected_codec == ac3;
      config.use_eac3 = selected_codec == eac3;
      config.packetDuration = 32;
      config.flags[config_t::HIGH_QUALITY] = true;
      config.flags[config_t::CUSTOM_SURROUND_PARAMS] = false;
      config.high_quality_dolby = high_quality;
    }
    return selected_codec;
  }

  std::string_view dolby_codec_name(dolby_codec_e codec) {
    using enum audio::dolby_codec_e;

    switch (codec) {
      case ac3:
        return "AC3";
      case eac3:
        return "EAC3";
      case opus:
      default:
        return "Opus";
    }
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
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
    // restore audio-sink if applicable
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
