/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once
#include <utility>

#include <boost/asio.hpp>

#include "audio.h"
#include "server/rtsp.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;
  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t>
    alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int
    start(session_t &session, const std::string &addr_string);
    void
    stop(session_t &session);
    void
    join(session_t &session);
    state_e
    state(session_t &session);
  }  // namespace session
}  // namespace stream
