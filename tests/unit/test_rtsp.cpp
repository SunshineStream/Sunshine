/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Verify RTSP response handlers through their common dispatcher signature.
 */

// local includes
// Include before test helpers so transitive atomic includes cannot hide C++ templates inside C linkage.
#include <src/compat/moonlight.h>

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <format>
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
#include <src/config.h>
#include <src/rtsp.h>
#include <src/video.h>

/**
 * @brief The Moonlight compatibility header can be included first before using the C RTSP parser.
 */
TEST(RtspHeaderTest, MoonlightCompatibilityHeaderCanPrecedeRtspParser) {
  char request[] = "OPTIONS rtsp://localhost RTSP/1.0\r\nCSeq: 42\r\n\r\n";
  RTSP_MESSAGE message {};
  ASSERT_EQ(parseRtspMessage(&message, request, sizeof(request) - 1), RTSP_ERROR_SUCCESS);
  const auto cleanup = util::fail_guard([&message]() {
    freeMessage(&message);
  });

  EXPECT_EQ(message.type, TYPE_REQUEST);
  EXPECT_EQ(message.sequenceNumber, 42);
  EXPECT_STREQ(message.message.request.command, "OPTIONS");
}

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
   * @brief Validate ANNOUNCE input before creating a stream.
   *
   * @param server RTSP server, unused when validation rejects the request.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Parsed ANNOUNCE request.
   */
  void cmd_announce(rtsp_server_t *server, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to PLAY through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_play(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);
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
 * @brief Reject invalid H.263+ resolutions before negotiating stream encryption.
 */
TEST(RtspAnnounceTest, ValidatesH263PlusDimensionsAfterApplyingResolutionCap) {
  using boost::asio::ip::tcp;

  auto &h263p = video::probed_codec_modes().h263p;
  auto &lan_encryption = config::stream.lan_encryption_mode;
  auto &max_width = config::video.max_stream_width;
  auto &max_height = config::video.max_stream_height;
  auto restore = util::fail_guard([&, old_h263p = h263p, old_encryption = lan_encryption, old_width = max_width, old_height = max_height] {
    h263p = old_h263p;
    lan_encryption = old_encryption;
    max_width = old_width;
    max_height = old_height;
  });
  h263p = 2;
  // Valid resolutions reach the encryption gate without starting a stream.
  lan_encryption = config::ENCRYPTION_MODE_MANDATORY;

  /**
   * @brief One ANNOUNCE request and the expected validation response.
   */
  struct announce_case_t {
    int format;  ///< Requested video codec.
    int width;  ///< Requested width in pixels.
    int height;  ///< Requested height in pixels.
    int cap_width;  ///< Maximum width in pixels, or zero to disable capping.
    int cap_height;  ///< Maximum height in pixels, or zero to disable capping.
    int status;  ///< Expected RTSP status code.
  };

  const std::array cases {
    announce_case_t {video::format_h263p, 640, 480, 0, 0, 403},
    announce_case_t {video::format_h263p, 1366, 768, 0, 0, 400},
    announce_case_t {video::format_h263p, 640, 478, 0, 0, 400},
    announce_case_t {video::format_h263p, 0, 480, 0, 0, 400},
    announce_case_t {video::format_h263p, 640, -4, 0, 0, 400},
    announce_case_t {video::format_h263p, 1920, 1080, 1366, 800, 400},
    announce_case_t {video::format_h263p, 1920, 1080, 1280, 720, 403},
    announce_case_t {video::format_h264, 1366, 768, 0, 0, 403},
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(std::format("{}x{} (cap {}x{}, codec {})", test_case.width, test_case.height, test_case.cap_width, test_case.cap_height, test_case.format));
    max_width = test_case.cap_width;
    max_height = test_case.cap_height;

    const auto sdp = std::format(
      "a=x-nv-audio.surround.numChannels:2\r\n"
      "a=x-nv-audio.surround.channelMask:3\r\n"
      "a=x-nv-audio.surround.AudioQuality:0\r\n"
      "a=x-nv-video[0].packetSize:1024\r\n"
      "a=x-nv-video[0].clientViewportWd:{}\r\n"
      "a=x-nv-video[0].clientViewportHt:{}\r\n"
      "a=x-nv-video[0].maxFPS:60\r\n"
      "a=x-nv-vqos[0].bw.maximumBitrateKbps:3000\r\n"
      "a=x-nv-video[0].videoEncoderSlicesPerFrame:1\r\n"
      "a=x-nv-video[0].maxNumReferenceFrames:1\r\n"
      "a=x-nv-vqos[0].bitStreamFormat:{}\r\n",
      test_case.width,
      test_case.height,
      test_case.format
    );
    auto message = std::format("ANNOUNCE rtsp://localhost RTSP/1.0\r\nCSeq: 42\r\nContent-Length: {}\r\n\r\n{}", sdp.size(), sdp);
    rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
    ASSERT_EQ(parseRtspMessage(request.get(), message.data(), message.size()), RTSP_ERROR_SUCCESS);

    boost::asio::io_context io;
    tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(acceptor.local_endpoint());
    auto server_socket = acceptor.accept();
    rtsp_stream::launch_session_t session {};
    rtsp_stream::cmd_announce(nullptr, server_socket, session, std::move(request));
    server_socket.shutdown(tcp::socket::shutdown_send);

    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    ASSERT_EQ(read_error, boost::asio::error::eof);
    EXPECT_TRUE(response.starts_with(std::format("RTSP/1.0 {} ", test_case.status))) << response;
  }
}
