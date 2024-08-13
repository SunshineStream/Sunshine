/**
 * @file src/server/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

#include "src/server/network.h"

namespace http {

  int
  init();
  int
  create_creds(const std::string &pkey, const std::string &cert);
  int
  save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false);

  int
  reload_user_creds(const std::string &file);
  bool
  download_file(const std::string &url, const std::string &file);
  std::string
  url_escape(const std::string &url);
  std::string
  url_get_host(const std::string &url);

  extern std::string unique_id;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
