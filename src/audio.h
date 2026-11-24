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
#include <cstddef>
#include <bitset>
#include <string_view>
#include <vector>

namespace stream {
  struct session_t;
}

namespace audio {
  constexpr std::size_t MAX_DOLBY_PACKET_SIZE = 2560;  ///< Maximum 640 kbit/s AC-3 access unit at 48 kHz.
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

    bool use_ac3 {false};  ///< Encode audio as AC-3 instead of Opus for this streaming session.
    bool use_eac3 {false};  ///< Encode audio as E-AC-3 instead of Opus for this streaming session.
    bool high_quality_dolby {false};  ///< Higher bitrate for Dolby only; ignored for Opus.
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

  /**
   * @brief Check whether a compatible optional FFmpeg AC-3 encoder is available.
   * @return True when the required FFmpeg DLLs and AC-3 encoder are available.
   */
  bool ac3_available();

  /**
   * @brief Check whether a compatible optional FFmpeg E-AC-3 encoder is available.
   * @return True when the required FFmpeg DLLs and E-AC-3 encoder are available.
   */
  bool eac3_available();

  /**
   * @brief Audio codec selected by RTSP Dolby offer/accept negotiation.
   */
  enum class dolby_codec_e {
    opus,  ///< Default Opus audio codec.
    ac3,  ///< Dolby Digital AC-3 bitstream.
    eac3  ///< Dolby Digital Plus E-AC-3 bitstream.
  };

  /**
   * @brief Configure a Dolby stream only for an exact, supported client acceptance.
   * @param config Audio configuration to update for the negotiated stream.
   * @param offered Codec offered by Sunshine, or empty when using Opus.
   * @param accepted Codec explicitly accepted by the client.
   * @param ac3_encoder_available Whether the AC-3 encoder is available.
   * @param eac3_encoder_available Whether the E-AC-3 encoder is available.
   * @param high_quality_requested Whether the client requested the higher Dolby bitrate in ANNOUNCE.
   * @return Selected codec, falling back to Opus when negotiation is unsupported.
   */
  dolby_codec_e configure_dolby_audio(
    config_t &config,
    std::string_view offered,
    std::string_view accepted,
    bool ac3_encoder_available,
    bool eac3_encoder_available,
    bool high_quality
  );

  /**
   * @brief Return the codec label for the RTSP audio-codec response header.
   * @param codec Selected audio codec.
   * @return RTSP codec label.
   */
  std::string_view dolby_codec_name(dolby_codec_e codec);

#ifdef SUNSHINE_TESTS
  /**
   * @brief Encode a Dolby frame directly for unit testing without audio capture.
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
  );
#endif

  /**
   * @brief Select a channel-aware bitrate for Dolby audio encoding.
   *
   * @param eac3 True to select E-AC-3; false to select AC-3.
   * @param high_quality Whether to use the higher Dolby bitrate.
   * @param channels Number of output audio channels (stereo or 5.1).
   * @return The selected bitrate in kbit/s, or 0 for unsupported channel layouts.
   */
  int dolby_bitrate_kbps(bool eac3, bool high_quality, int channels);
}  // namespace audio
