/**
 * @file tests/unit/test_nvhttp_https.cpp
 * @brief Verify HTTPS callbacks retain the guard protecting server lifetime.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <istream>
#include <mutex>
#include <string>
#include <thread>

// lib includes
#include <boost/asio/ssl.hpp>

// local includes
#include <src/crypto.h>
#include <src/nvhttp.h>

namespace {
  using namespace std::chrono_literals;
  using boost::asio::ip::tcp;

  /**
   * @brief Run real HTTPS callbacks against an isolated loopback listener.
   */
  struct HttpsHandlerLifetimeTest: BaseTest {
    /**
     * @brief Create credentials and start the production HTTPS accept handler.
     */
    void SetUp() override {
      BaseTest::SetUp();
      const auto credentials = crypto::gen_creds("Sunshine HTTPS lifetime test", 2048);
      certificate_pem = credentials.x509;
      certificate_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_lifetime_cert.pem";
      private_key_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "https_lifetime_key.pem";
      std::ofstream {certificate_file} << credentials.x509;
      std::ofstream {private_key_file} << credentials.pkey;

      server = nvhttp::test_support::make_https_server(certificate_file.string(), private_key_file.string(), handler_scope, verifier());
      server->io_service = std::make_shared<boost::asio::io_context>();
      server->config.address = listen_address();
      server->config.port = 0;
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server->io_service->poll();
      ASSERT_NE(port, 0);
    }

    /**
     * @brief Select the listener address used by a network fixture.
     * @return IPv4 loopback address for lifetime tests.
     */
    virtual std::string listen_address() {
      return "127.0.0.1";
    }

    /**
     * @brief Supply optional client authorization for a production HTTPS fixture.
     * @return Empty verifier for lifetime tests.
     */
    virtual std::function<int(SSL *)> verifier() {
      return {};
    }

    /**
     * @brief Drain canceled callbacks and remove the generated credentials.
     */
    void TearDown() override {
      if (server) {
        server->stop();
        server->io_service->restart();
        server->io_service->poll();
        server.reset();
      }
      if (stop_thread.joinable()) {
        stop_thread.join();
      }
      std::error_code cleanup_error;
      std::filesystem::remove(certificate_file, cleanup_error);
      std::filesystem::remove(private_key_file, cleanup_error);
      BaseTest::TearDown();
    }

    /**
     * @brief Check that stopping the scope runner waits until the error callback exits.
     *
     * @param callback_error Error delivered by the real accept or TLS handshake operation.
     */
    void check_handler_guard(const boost::system::error_code &callback_error) {
      EXPECT_TRUE(callback_error);
      ++error_callbacks;
      std::promise<void> stopping;
      auto stopping_future = stopping.get_future();
      std::promise<void> stopped;
      stopped_future = stopped.get_future();
      stop_thread = std::jthread([this, stopping = std::move(stopping), stopped = std::move(stopped)]() mutable {
        stopping.set_value();
        handler_scope->stop();
        stopped.set_value();
      });
      stopping_future.wait();
      EXPECT_EQ(stopped_future.wait_for(50ms), std::future_status::timeout);
    }

    std::unique_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Server running production accept and handshake callbacks.
    std::shared_ptr<SimpleWeb::ScopeRunner> handler_scope;  ///< Guard used by the server destructor.
    std::jthread stop_thread;  ///< Concurrent attempt to stop the handler scope.
    std::future<void> stopped_future;  ///< Signals that all callback guards have been released.
    unsigned short port = 0;  ///< Assigned loopback port.
    int error_callbacks = 0;  ///< Number of errors delivered while testing a handler.
    std::string certificate_pem;  ///< Canonical certificate presented by the first test client.
    std::filesystem::path certificate_file;  ///< Generated server certificate.
    std::filesystem::path private_key_file;  ///< Generated server private key.
  };

  /**
   * @brief Exercise request authorization on an already-open TLS connection.
   */
  struct HttpsRevocationTest: HttpsHandlerLifetimeTest {
    /**
     * @brief Return the current authorization decision for the presented certificate.
     * @return Verifier shared with the fixture's authorization state.
     */
    std::function<int(SSL *)> verifier() override {
      return [this](const SSL *ssl) {
        ++verification_count;
#if OPENSSL_VERSION_MAJOR >= 3
        crypto::x509_t certificate {SSL_get1_peer_certificate(ssl)};
#else
        crypto::x509_t certificate {SSL_get_peer_certificate(ssl)};
#endif
        std::lock_guard lock {identity_mutex};
        last_certificate = certificate ? crypto::pem(certificate) : std::string {};
        return authorized.load() && !last_certificate.empty() && last_certificate != rejected_certificate ? 1 : 0;
      };
    }

    /**
     * @brief Accept connections addressed to different local loopback interfaces.
     * @return IPv4 wildcard listener address.
     */
    std::string listen_address() override {
      return "0.0.0.0";
    }

    /**
     * @brief Send one keep-alive request and consume its complete response.
     * @param client Connected TLS client.
     * @return HTTP response status line.
     */
    static std::string request(boost::asio::ssl::stream<tcp::socket> &client) {
      const std::string wire = "GET /authorization-test HTTP/1.1\r\nHost: localhost\r\n\r\n";
      boost::asio::write(client, boost::asio::buffer(wire));
      boost::asio::streambuf buffer;
      boost::asio::read_until(client, buffer, "\r\n\r\n");
      std::istream headers {&buffer};
      std::string status;
      std::getline(headers, status);
      std::string line;
      std::size_t content_length = 0;
      while (std::getline(headers, line) && line != "\r") {
        if (line.starts_with("Content-Length: ")) {
          content_length = std::stoul(line.substr(16));
        }
      }
      if (buffer.size() < content_length) {
        boost::asio::read(client, buffer, boost::asio::transfer_exactly(content_length - buffer.size()));
      }
      return status;
    }

    /**
     * @brief Run the production HTTPS listener with a protected test route.
     */
    void SetUp() override {
      HttpsHandlerLifetimeTest::SetUp();
      // after_bind wraps handlers, so use the wrapped default route configured by the factory.
      io_thread = std::jthread([this]() {
        server->io_service->run();
      });
    }

    /**
     * @brief Stop the IO thread before draining callbacks in the parent fixture.
     */
    void TearDown() override {
      server->stop();
      if (io_thread.joinable()) {
        io_thread.join();
      }
      HttpsHandlerLifetimeTest::TearDown();
    }

    std::mutex identity_mutex;  ///< Protects certificate observations across fixture and IO threads.
    std::string last_certificate;  ///< Identity checked by the most recent production callback.
    std::string rejected_certificate;  ///< Identity revoked while a connection remains open.
    std::atomic_bool authorized {true};  ///< Simulated paired-client authorization.
    std::atomic_int verification_count {0};  ///< Verifications across handshake and requests.
    std::jthread io_thread;  ///< Thread dispatching the real HTTPS callbacks.
  };

  /**
   * @brief Revocation applies to keep-alive requests even after a successful handshake.
   */
  TEST_F(HttpsRevocationTest, RechecksAuthorizationBeforeEveryKeepAliveRequest) {
    boost::asio::io_context client_io;
    boost::asio::ssl::context tls {boost::asio::ssl::context::tls_client};
    tls.set_verify_mode(boost::asio::ssl::verify_none);
    tls.use_certificate_chain_file(certificate_file.string());
    tls.use_private_key_file(private_key_file.string(), boost::asio::ssl::context::pem);
    boost::asio::ssl::stream<tcp::socket> client {client_io, tls};
    client.lowest_layer().connect({boost::asio::ip::address_v4::loopback(), port});
    client.handshake(boost::asio::ssl::stream_base::client);

    EXPECT_NE(request(client).find("200 OK"), std::string::npos);
    authorized = false;
    EXPECT_NE(request(client).find("401 Unauthorized"), std::string::npos);
    EXPECT_GE(verification_count.load(), 3);
  }

  /**
   * @brief Connections sharing a remote endpoint retain distinct local-interface identities.
   */
  TEST_F(HttpsRevocationTest, UsesBothEndpointsWhenReauthorizingKeepAliveClients) {
    boost::asio::io_context io;
    boost::asio::ssl::context first_tls {boost::asio::ssl::context::tls_client};
    first_tls.set_verify_mode(boost::asio::ssl::verify_none);
    first_tls.use_certificate_chain_file(certificate_file.string());
    first_tls.use_private_key_file(private_key_file.string(), boost::asio::ssl::context::pem);
    boost::asio::ssl::stream<tcp::socket> first {io, first_tls};
    first.lowest_layer().open(tcp::v4());
    first.lowest_layer().set_option(tcp::socket::reuse_address(true));
    first.lowest_layer().bind({boost::asio::ip::address_v4::loopback(), 0});
    first.lowest_layer().connect({boost::asio::ip::make_address_v4("127.0.0.1"), port});
    first.handshake(boost::asio::ssl::stream_base::client);

    const auto second_credentials = crypto::gen_creds("Sunshine second client", 2048);
    const auto second_cert = certificate_file.string() + ".second";
    const auto second_key = private_key_file.string() + ".second";
    const auto cleanup = util::fail_guard([&second_cert, &second_key]() {
      std::error_code ec;
      std::filesystem::remove(second_cert, ec);
      std::filesystem::remove(second_key, ec);
    });
    std::ofstream {second_cert} << second_credentials.x509;
    std::ofstream {second_key} << second_credentials.pkey;
    boost::asio::ssl::context second_tls {boost::asio::ssl::context::tls_client};
    second_tls.set_verify_mode(boost::asio::ssl::verify_none);
    second_tls.use_certificate_chain_file(second_cert);
    second_tls.use_private_key_file(second_key, boost::asio::ssl::context::pem);
    boost::asio::ssl::stream<tcp::socket> second {io, second_tls};
    second.lowest_layer().open(tcp::v4());
    second.lowest_layer().set_option(tcp::socket::reuse_address(true));
    boost::system::error_code bind_error;
    second.lowest_layer().bind(first.lowest_layer().local_endpoint(), bind_error);
    if (bind_error) {
      GTEST_SKIP() << "Platform cannot reuse a source endpoint across destinations: " << bind_error.message();
    }
    second.lowest_layer().connect({boost::asio::ip::make_address_v4("127.0.0.2"), port});
    second.handshake(boost::asio::ssl::stream_base::client);

    // Finish the server-side handshake callback before observing request-local identity.
    EXPECT_NE(request(second).find("200 OK"), std::string::npos);
    {
      std::lock_guard lock {identity_mutex};
      EXPECT_EQ(last_certificate, second_credentials.x509);
    }
    EXPECT_NE(request(first).find("200 OK"), std::string::npos);
    {
      std::lock_guard lock {identity_mutex};
      EXPECT_EQ(last_certificate, certificate_pem);
      rejected_certificate = certificate_pem;
    }
    EXPECT_NE(request(first).find("401 Unauthorized"), std::string::npos);
    EXPECT_NE(request(second).find("200 OK"), std::string::npos);
    {
      std::lock_guard lock {identity_mutex};
      EXPECT_EQ(last_certificate, second_credentials.x509);
    }
  }

  /**
   * @brief Canceling a pending accept holds the server guard through its error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, AcceptErrorRetainsGuardUntilCallbackReturns) {
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    server->stop();
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief A failed TLS handshake holds the server guard through its error callback.
   */
  TEST_F(HttpsHandlerLifetimeTest, HandshakeErrorRetainsGuardUntilCallbackReturns) {
    server->on_error = [this](auto, const auto &callback_error) {
      check_handler_guard(callback_error);
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    client.close();
    server->io_service->run_for(2s);
    ASSERT_EQ(error_callbacks, 1);
    ASSERT_TRUE(stopped_future.valid());
    EXPECT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
  }

  /**
   * @brief A stopped server scope prevents a canceled accept from dispatching an error.
   */
  TEST_F(HttpsHandlerLifetimeTest, StoppedScopeSkipsAcceptCallback) {
    server->on_error = [this](auto, const auto &) {
      ++error_callbacks;
    };
    handler_scope->stop();
    server->stop();
    server->io_service->run_for(2s);
    EXPECT_EQ(error_callbacks, 0);
  }

  /**
   * @brief A stopped server scope prevents an outstanding handshake from dispatching an error.
   */
  TEST_F(HttpsHandlerLifetimeTest, StoppedScopeSkipsHandshakeCallback) {
    server->on_error = [this](auto, const auto &) {
      ++error_callbacks;
    };
    boost::asio::io_context client_io;
    tcp::socket client {client_io};
    client.connect({boost::asio::ip::address_v4::loopback(), port});
    server->io_service->poll();
    handler_scope->stop();
    client.close();
    server->io_service->run_for(2s);
    EXPECT_EQ(error_callbacks, 0);
  }
}  // namespace
