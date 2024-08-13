/**
 * @file src/server/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

#include <functional>
#include <string>

#include "src/thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  void
  start();
}  // namespace confighttp

// mime types map
const std::map<std::string, std::string> mime_types = {
  { "css", "text/css" },
  { "gif", "image/gif" },
  { "htm", "text/html" },
  { "html", "text/html" },
  { "ico", "image/x-icon" },
  { "jpeg", "image/jpeg" },
  { "jpg", "image/jpeg" },
  { "js", "application/javascript" },
  { "json", "application/json" },
  { "png", "image/png" },
  { "svg", "image/svg+xml" },
  { "ttf", "font/ttf" },
  { "txt", "text/plain" },
  { "woff2", "font/woff2" },
  { "xml", "text/xml" },
};
