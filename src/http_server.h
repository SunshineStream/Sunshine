/**
 * @file src/http_server.h
 * @brief Shared request limits for the GameStream HTTP servers.
 */
#pragma once

// standard includes
#include <cstddef>

namespace http {
  constexpr std::size_t MAX_GAMESTREAM_REQUEST_SIZE = 64 * 1024;  ///< Maximum GameStream request size in bytes.

  /**
   * @brief Bound GameStream requests before routing or authentication.
   * @details GameStream requests use the query string, including the pairing certificate, and have no body.
   * The error handler ends a connection that fails while it is read; the server calls it without checking it is set
   * when a chunked body is cut short.
   * @param server HTTP or HTTPS server that handles GameStream requests.
   */
  template<class Server>
  void configure_gamestream_server(Server &server) {
    server.config.max_request_streambuf_size = MAX_GAMESTREAM_REQUEST_SIZE;
    server.on_error = [](auto, const auto &) {
      // Nothing to report: the connection is closed with its session.
    };
  }
}  // namespace http
