/**
 * @file src/server/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

#include <tuple>
#include <utility>

#include <boost/asio.hpp>

#include <enet/enet.h>

#include "src/utility.h"

namespace net {
  void
  free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t
  map_port(int port);

  using host_t = util::safe_ptr<ENetHost, free_host>;
  using peer_t = ENetPeer *;
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  net_e
  from_enum_string(const std::string_view &view);
  std::string_view
  to_enum_string(net_e net);

  net_e
  from_address(const std::string_view &view);

  host_t
  host_create(af_e af, ENetAddress &addr, std::size_t peers, std::uint16_t port);

  /**
   * @brief Get the address family enum value from a string.
   * @param view The config option value.
   * @return The address family enum value.
   */
  af_e
  af_from_enum_string(const std::string_view &view);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view
  af_to_any_address_string(af_e af);

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address
  normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string
  addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string
  addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int
  encryption_mode_for_address(boost::asio::ip::address address);
}  // namespace net
