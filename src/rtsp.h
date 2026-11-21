/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// local includes
#include "crypto.h"
#include "thread_safe.h"

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;  ///< GameStream base-port offset used for the RTSP setup listener.

#pragma pack(push, 1)

  /**
   * @brief Encrypted RTSP message header used by GameStream.
   */
  struct encrypted_rtsp_header_t {
    // We set the MSB in encrypted RTSP messages to allow format-agnostic
    // parsing code to be able to tell encrypted from plaintext messages.
    static constexpr std::uint32_t ENCRYPTED_MESSAGE_TYPE_BIT = 0x80000000;  ///< Protocol or platform constant for encrypted message type bit.

    /**
     * @brief Return a pointer to the protocol payload following the packet header.
     *
     * @return Parsed or serialized payload data.
     */
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    /**
     * @brief Decode the RTSP interleaved-frame payload length.
     *
     * @return Payload byte count with the encrypted-message flag masked out.
     */
    std::uint32_t payload_length() const {
      return util::endian::big<std::uint32_t>(typeAndLength) & ~ENCRYPTED_MESSAGE_TYPE_BIT;
    }

    /**
     * @brief Inspect the encrypted-message flag without modifying the wire header.
     *
     * @return True when the encrypted-message flag is set.
     */
    bool is_encrypted() const {
      return !!(util::endian::big<std::uint32_t>(typeAndLength) & ENCRYPTED_MESSAGE_TYPE_BIT);
    }

    // This field is the length of the payload + ENCRYPTED_MESSAGE_TYPE_BIT in big-endian
    std::uint32_t typeAndLength;  ///< Type and length.

    // This field is the number used to initialize the bottom 4 bytes of the AES IV in big-endian
    std::uint32_t sequenceNumber;  ///< Sequence number.

    // This field is the AES GCM authentication tag
    std::array<std::uint8_t, 16> tag;  ///< Authentication tag appended to the encrypted payload.
  };

#pragma pack(pop)

  static_assert(sizeof(encrypted_rtsp_header_t) == 24);
  static_assert(offsetof(encrypted_rtsp_header_t, tag) == 8);

  /**
   * @brief RTSP launch session state shared with stream setup.
   */
  struct launch_session_t {
    /**
     * @brief Audio-related capabilities and codec choice exchanged during RTSP setup.
     */
    struct audio_negotiation_t {
      bool continuous_audio {};  ///< Whether audio packets continue during silence.
      std::string codec_offered;  ///< Dolby codec offered to the client, or empty when using Opus.
    };

    uint32_t id;  ///< RTSP launch-session identifier assigned before stream startup.

    crypto::aes_t gcm_key;  ///< AES-GCM key negotiated for encrypted RTSP messages.
    crypto::aes_t iv;  ///< Initial RTSP AES-GCM IV supplied by the client.

    std::string av_ping_payload;  ///< AV ping payload.
    uint32_t control_connect_data;  ///< Client-provided token used when connecting the control channel.

    bool host_audio;  ///< Whether host audio should be played locally.
    std::string unique_id;  ///< Moonlight client unique identifier for this launch request.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int fps;  ///< Requested video frame rate.
    int gcmap;  ///< Game controller mapping requested by the client.
    int appid;  ///< Application ID requested for launch or resume.
    int surround_info;  ///< Encoded GameStream surround-sound capability flags.
    std::string surround_params;  ///< Client-provided surround-sound layout parameters.
    audio_negotiation_t audio_negotiation;  ///< Audio stream negotiation state.
    bool enable_hdr;  ///< Whether HDR streaming is requested.
    bool enable_sops;  ///< Whether sequence output protection is requested.
    std::string client_name;  ///< Friendly client name from initial pairing.

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;  ///< AES-GCM cipher used once encrypted RTSP is negotiated.
    std::string rtsp_url_scheme;  ///< URL scheme selected by the RTSP SETUP flow.
    uint32_t rtsp_iv_counter;  ///< Counter value mixed into encrypted RTSP IVs.
    std::string client_cert;  ///< PEM certificate for the paired Moonlight client.
  };

  /**
   * @brief Queue a launch session until the RTSP client connects.
   *
   * @param launch_session Session state prepared by the GameStream launch handler.
   */
  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  /**
   * @brief Terminate active sessions associated with a client certificate.
   *
   * @param cert Certificate data or object used by the operation.
   */
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
