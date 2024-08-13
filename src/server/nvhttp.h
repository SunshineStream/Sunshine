/**
 * @file src/server/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <string>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "src/thread_safe.h"

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void
  start();

  /**
   * @brief Compare the user supplied pin to the Moonlight pin.
   * @param pin The user supplied pin.
   * @param name The user supplied name.
   * @return `true` if the pin is correct, `false` otherwise.
   * @examples
   * bool pin_status = nvhttp::pin("1234", "laptop");
   * @examples_end
   */
  bool
  pin(std::string pin, std::string name);

  /**
   * @brief Remove single client.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   */
  int
  unpair_client(std::string uniqueid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * boost::property_tree::ptree clients = nvhttp::get_all_clients();
   * @examples_end
   */
  boost::property_tree::ptree
  get_all_clients();

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void
  erase_all_clients();
}  // namespace nvhttp
