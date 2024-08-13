/**
 * @file src/audio.h
 * @brief Declarations for audio capture and encoding.
 */
#pragma once

#include "thread_safe.h"
#include "utility.h"

namespace audio {
  enum stream_config_e : int {
    STEREO,  ///< Stereo
    HIGH_STEREO,  ///< High stereo
    SURROUND51,  ///< Surround 5.1
    HIGH_SURROUND51,  ///< High surround 5.1
    SURROUND71,  ///< Surround 7.1
    HIGH_SURROUND71,  ///< High surround 7.1
    MAX_STREAM_CONFIG  ///< Maximum audio stream configuration
  };

  struct opus_stream_config_t {
    std::int32_t sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    const std::uint8_t *mapping;
    int bitrate;
  };

  struct stream_params_t {
    int channelCount;
    int streams;
    int coupledStreams;
    std::uint8_t mapping[8];
  };

  extern opus_stream_config_t stream_configs[MAX_STREAM_CONFIG];

  struct config_t {
    enum flags_e : int {
      HIGH_QUALITY,  ///< High quality audio
      HOST_AUDIO,  ///< Host audio
      CUSTOM_SURROUND_PARAMS,  ///< Custom surround parameters
      MAX_FLAGS  ///< Maximum number of flags
    };

    int packetDuration;
    int channels;
    int mask;

    stream_params_t customStreamParams;

    std::bitset<MAX_FLAGS> flags;
  };

  using buffer_t = util::buffer_t<std::uint8_t>;
  using packet_t = std::pair<void *, buffer_t>;
  void
  capture(safe::mail_t mail, config_t config, void *channel_data);
}  // namespace audio
