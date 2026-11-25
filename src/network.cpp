/**
 * @file src/network.cpp
 * @brief Definitions for networking related functions.
 */
// standard includes
#include <algorithm>
#include <sstream>

// local includes
#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  /**
   * @brief IPv4 loopback networks used for local-client classification.
   */
  std::vector pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  /**
   * @brief IPv4 private and link-local networks used for LAN classification.
   */
  std::vector lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),
    ip::make_network_v4("172.16.0.0/12"sv),
    ip::make_network_v4("10.0.0.0/8"sv),
    ip::make_network_v4("100.64.0.0/10"sv),
    ip::make_network_v4("169.254.0.0/16"sv),
  };

  /**
   * @brief IPv6 loopback networks used for local-client classification.
   */
  std::vector pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  /**
   * @brief IPv6 private and link-local networks used for LAN classification.
   */
  std::vector lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  /**
   * @brief Convert configuration text to a network enum value.
   */
  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  /**
   * @brief Classify an address using the configured loopback and LAN ranges without modifying them.
   */
  net_e from_address(const std::string_view &view) {
    if (auto addr = normalize_address(ip::make_address(view)); addr.is_v6()) {
      for (const auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (const auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }
    } else {
      for (const auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (const auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  /**
   * @brief Convert a network enum value to configuration text.
   */
  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  af_e af_from_enum_string(const std::string_view &view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    // avoid warning
    return BOTH;
  }

  af_e get_effective_address_family(const af_e af) {
    if (af != BOTH || config::sunshine.bind_address.empty()) {
      return af;
    }

    boost::system::error_code ec;
    const auto bind_address = ip::make_address(config::sunshine.bind_address, ec);
    return !ec && bind_address.is_v4() ? IPV4 : af;
  }

  std::string_view af_to_any_address_string(const af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    // avoid warning
    return "::"sv;
  }

  std::string get_bind_address(const af_e af) {
    // If bind_address is configured, use it
    if (!config::sunshine.bind_address.empty()) {
      return config::sunshine.bind_address;
    }

    // Otherwise use the wildcard address for the given address family
    return std::string(af_to_any_address_string(af));
  }

  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      std::stringstream ss;
      ss << '[' << address.to_string() << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  std::string get_bind_address_url_host() {
    if (config::sunshine.bind_address.empty()) {
      return "localhost";
    }

    boost::system::error_code ec;
    const auto bind_address = ip::make_address(config::sunshine.bind_address, ec);
    return ec ? config::sunshine.bind_address : addr_to_url_escaped_string(bind_address);
  }

  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  bool set_enet_address(ENetAddress &addr, const std::string &host, std::uint16_t port) {
    boost::system::error_code ec;
    const auto address = ip::make_address(host, ec);
    if (ec) {
      // Not a numeric address, so resolve the host name through ENet
      return enet_address_set_host(&addr, host.c_str()) == 0 && enet_address_set_port(&addr, port) == 0;
    }

    // Numeric addresses are converted directly because ENet's resolver uses AI_ADDRCONFIG,
    // which rejects IPv6 addresses such as "::" on hosts without a non-loopback IPv6 address.
    ip::udp::endpoint endpoint {address, port};
    return enet_address_set_address(&addr, endpoint.data(), static_cast<socklen_t>(endpoint.size())) == 0;
  }

  /**
   * @brief Create an ENet host with the requested address family.
   */
  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port) {
    static std::once_flag enet_init_flag;
    std::call_once(enet_init_flag, []() {
      enet_initialize();
    });

    af = get_effective_address_family(af);
    const auto bind_addr = net::get_bind_address(af);
    if (!set_enet_address(addr, bind_addr, port)) {
      BOOST_LOG(error) << "Failed to resolve ENet bind address ["sv << bind_addr << ']';
      return host_t {};
    }

    // Maximum of 128 clients, which should be enough for anyone
    auto host = host_t {enet_host_create(af == IPV4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};
    if (!host) {
      BOOST_LOG(error) << "Failed to create ENet host on ["sv << bind_addr << "]:"sv << port;
      return host;
    }

    // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
    enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);

    return host;
  }

  /**
   * @brief Destroy an ENet host allocated by host_create().
   */
  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname) {
    // Start with the unmodified hostname
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      // Replace any spaces with dashes
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(instancename[i]) && instancename[i] != '-') {
        // Stop at the first invalid character
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "Sunshine";
  }
}  // namespace net
