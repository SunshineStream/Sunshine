/**
 * @file tests/unit/test_http_request_limits.cpp
 * @brief Regressions for the GameStream request limits applied before routing or authentication.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <string>

// lib includes
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include <src/http_server.h>

namespace {
  using namespace std::chrono_literals;
  using boost::asio::ip::tcp;

  /**
   * @brief Exercise the actual HTTP reader on an isolated loopback listener.
   */
  struct HttpRequestLimitsTest: BaseTest {
    /**
     * @brief Bind an ephemeral port using an externally driven event loop.
     */
    void SetUp() override {
      BaseTest::SetUp();
      http::configure_gamestream_server(server);
      server.io_service = std::make_shared<boost::asio::io_context>();
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.timeout_request = 1;
      server.config.timeout_content = 1;
      server.resource["^/serverinfo$"]["GET"] = [](auto response, auto) {
        response->write("ok");
      };
      server.start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server.io_service->poll();
      ASSERT_NE(port, 0);
    }

    /**
     * @brief Drain canceled callbacks before destroying the server.
     */
    void TearDown() override {
      server.stop();
      server.io_service->restart();
      server.io_service->poll();
      BaseTest::TearDown();
    }

    /**
     * @brief Send a request, optionally splitting its body across asynchronous reads.
     * @param request Initial request bytes.
     * @param fragment Additional bytes sent after the first read has run.
     * @return Bytes the server returned before closing or waiting for more input.
     */
    std::string exchange(const std::string &request, const std::string &fragment = {}) {
      boost::asio::io_context client_io;
      tcp::socket client {client_io};
      client.connect({boost::asio::ip::address_v4::loopback(), port});
      boost::asio::write(client, boost::asio::buffer(request));
      if (!fragment.empty()) {
        server.io_service->restart();
        server.io_service->run_for(30ms);
        boost::asio::write(client, boost::asio::buffer(fragment));
      }
      client.shutdown(tcp::socket::shutdown_send);
      server.io_service->restart();
      server.io_service->run_for(100ms);
      client.non_blocking(true);
      std::string response;
      std::array<char, 4096> buffer {};
      boost::system::error_code ec;
      for (;;) {
        const auto count = client.read_some(boost::asio::buffer(buffer), ec);
        response.append(buffer.data(), count);
        if (ec) {
          break;
        }
      }
      return response;
    }

    /**
     * @brief Verify the listener still serves an ordinary request after malformed input.
     */
    void expect_healthy() {
      EXPECT_NE(exchange("GET /serverinfo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").find("200 OK"), std::string::npos);
    }

    SimpleWeb::Server<SimpleWeb::HTTP> server;  ///< Actual dependency reader under test.
    unsigned short port = 0;  ///< Assigned loopback port.
  };

  TEST_F(HttpRequestLimitsTest, TruncatedChunkDoesNotStopTheServer) {
    EXPECT_NO_THROW(exchange("GET /serverinfo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nA"));
    expect_healthy();
  }

  TEST_F(HttpRequestLimitsTest, TruncatedChunkAfterSplitReadDoesNotStopTheServer) {
    EXPECT_NO_THROW(exchange("GET /serverinfo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n", "A"));
    expect_healthy();
  }

  TEST_F(HttpRequestLimitsTest, BoundsHeadersWithoutRejectingPairingSizedQueries) {
    EXPECT_NE(exchange("GET /serverinfo?clientcert=" + std::string(8192, 'a') + " HTTP/1.1\r\nConnection: close\r\n\r\n").find("200 OK"), std::string::npos);
    EXPECT_EQ(exchange("GET /serverinfo HTTP/1.1\r\nX-Large: " + std::string(http::MAX_GAMESTREAM_REQUEST_SIZE, 'a') + "\r\n\r\n").find("200 OK"), std::string::npos);
    expect_healthy();
  }

  TEST_F(HttpRequestLimitsTest, BoundsRequestBodies) {
    const auto body = std::string(http::MAX_GAMESTREAM_REQUEST_SIZE + 1, 'a');
    EXPECT_EQ(exchange("GET /serverinfo HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body).find("200 OK"), std::string::npos);
    expect_healthy();
  }
}  // namespace
