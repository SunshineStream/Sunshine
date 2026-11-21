/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Verify RTSP response handlers through their common dispatcher signature.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

// local includes
#include <src/audio.h>
#include <src/rtsp.h>

/**
 * @brief Const RTSP wire headers decode network-order lengths independently of encryption flags.
 */
TEST(RtspHeaderTest, ConstAccessorsPreserveTheSerializedHeader) {
  using rtsp_stream::encrypted_rtsp_header_t;
  for (const auto length : {0U, 1U, 0x7fffffffU}) {
    for (const bool encrypted : {false, true}) {
      const auto flags = encrypted ? encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT : 0U;
      const encrypted_rtsp_header_t header {util::endian::big(length | flags), 0, {}};
      EXPECT_EQ(header.payload_length(), length);
      EXPECT_EQ(header.is_encrypted(), encrypted);
      EXPECT_EQ(header.typeAndLength, util::endian::big(length | flags));
    }
  }
}

namespace rtsp_stream {
  class rtsp_server_t;

  /**
   * @brief Release a parsed RTSP message and its allocation.
   * @param message Message to release.
   */
  void free_msg(PRTSP_MESSAGE message);

  /**
   * @brief Owning RTSP message type used by the dispatcher.
   */
  using msg_t = util::safe_ptr<RTSP_MESSAGE, &free_msg>;

  /**
   * @brief Respond to OPTIONS through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_option(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to DESCRIBE through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_describe(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to PLAY through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_play(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);
  /**
   * @brief Send the successful ANNOUNCE response through the production response path.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param sequence_number ANNOUNCE request sequence number.
   * @param selected_audio_codec Codec accepted during negotiation.
   */
  void respond_announce_success(boost::asio::ip::tcp::socket &sock, launch_session_t &session, int sequence_number, audio::dolby_codec_e selected_audio_codec);
}  // namespace rtsp_stream

/**
 * @brief All three handlers respond with the request's sequence even without a server instance.
 */
TEST(RtspResponseTest, CommonDispatcherPreservesResponsesWithoutServerContext) {
  using boost::asio::ip::tcp;
  const std::array handlers {
    &rtsp_stream::cmd_option,
    &rtsp_stream::cmd_describe,
    &rtsp_stream::cmd_play,
  };

  for (const auto handler : handlers) {
    boost::asio::io_context io;
    tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(acceptor.local_endpoint());
    auto server_socket = acceptor.accept();

    rtsp_stream::launch_session_t session {};
    rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
    request->sequenceNumber = 42;
    handler(nullptr, server_socket, session, std::move(request));
    server_socket.shutdown(tcp::socket::shutdown_send);

    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    ASSERT_EQ(read_error, boost::asio::error::eof);
    EXPECT_TRUE(response.starts_with("RTSP/1.0 200 OK\r\n"));
    EXPECT_NE(response.find("CSeq: 42\r\n"), std::string::npos);

    if (handler == &rtsp_stream::cmd_describe) {
      EXPECT_NE(response.find("a=x-ss-general.featureFlags:"), std::string::npos);
      EXPECT_NE(response.find("a=x-ss-general.encryptionSupported:"), std::string::npos);
      EXPECT_NE(response.find("a=fmtp:97 surround-params="), std::string::npos);
    } else {
      EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    }
  }
}

/**
 * @brief Surround mappings compensate for the client swap over the channels of each layout, so every
 * speaker lands on its own position once the client applies its rotation.
 */
TEST(RtspDescribeTest, SurroundParamsCompensateClientRotationForEveryStreamConfig) {
  using boost::asio::ip::tcp;

  boost::asio::io_context io;
  tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
  tcp::socket client {io};
  client.connect(acceptor.local_endpoint());
  auto server_socket = acceptor.accept();

  rtsp_stream::launch_session_t session {};
  rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
  rtsp_stream::cmd_describe(nullptr, server_socket, session, std::move(request));
  server_socket.shutdown(tcp::socket::shutdown_send);

  std::string response;
  boost::system::error_code read_error;
  boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
  ASSERT_EQ(read_error, boost::asio::error::eof);

  const std::string params_prefix = "a=fmtp:97 surround-params=";
  std::vector<std::string> advertised;
  for (auto start = response.find(params_prefix); start != std::string::npos;
       start = response.find(params_prefix, start + params_prefix.size())) {
    const auto end = response.find('\n', start);
    advertised.push_back(response.substr(start + params_prefix.size(), end - start - params_prefix.size()));
  }

  // moonlight-common-c moves the last speaker of a surround layout to index 3 and shifts the rest up,
  // so the compensation must cover the channels of the layout being advertised rather than a fixed range.
  const std::vector<std::pair<int, std::string>> expected {
    {audio::STEREO, "21101"},
    {audio::HIGH_STEREO, "21101"},
    {audio::SURROUND51, "642012453"},
    {audio::HIGH_SURROUND51, "660012345"},
    {audio::SURROUND71, "85301245673"},
    {audio::HIGH_SURROUND71, "88001234567"},
  };

  ASSERT_EQ(advertised.size(), expected.size());
  for (std::size_t x = 0; x < expected.size(); ++x) {
    EXPECT_EQ(advertised[x], expected[x].second) << "stream config " << expected[x].first;
  }
}

/**
 * @brief ANNOUNCE success responses expose the negotiated Dolby codec while keeping legacy Opus headers unchanged.
 */
TEST(RtspAnnounceResponseTest, SerializesDolbyCodecAndLegacyOpusResponses) {
  using boost::asio::ip::tcp;

  const std::array negotiations {
    std::pair {audio::dolby_codec_e::ac3, std::string_view {"AC3"}},
    std::pair {audio::dolby_codec_e::eac3, std::string_view {"EAC3"}},
    std::pair {audio::dolby_codec_e::opus, std::string_view {}},
  };

  for (const auto &[codec, expected_header] : negotiations) {
    SCOPED_TRACE(expected_header.empty() ? "legacy Opus client" : std::string(expected_header));
    boost::asio::io_context io;
    tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(acceptor.local_endpoint());
    auto server_socket = acceptor.accept();

    rtsp_stream::launch_session_t session {};
    rtsp_stream::respond_announce_success(server_socket, session, 73, codec);
    server_socket.shutdown(tcp::socket::shutdown_send);

    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    ASSERT_EQ(read_error, boost::asio::error::eof);
    EXPECT_TRUE(response.starts_with("RTSP/1.0 200 OK\r\n"));
    EXPECT_NE(response.find("CSeq: 73\r\n"), std::string::npos);

    const auto codec_header = response.find("X-SS-Audio-Codec:");
    if (expected_header.empty()) {
      EXPECT_EQ(codec_header, std::string::npos);
      EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    } else {
      ASSERT_NE(codec_header, std::string::npos);
      EXPECT_NE(response.find(std::string("X-SS-Audio-Codec: ") + std::string(expected_header) + "\r\n"), std::string::npos);
    }
  }
}
