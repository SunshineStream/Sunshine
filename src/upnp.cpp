/**
 * @file src/upnp.cpp
 * @brief Definitions for UPnP port mapping.
 */
// standard includes
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stddef.h>  // workaround for type_t error in miniupnpc 2.3.3, see https://github.com/miniupnp/miniupnp/commit/e263ab6f56c382e10fed31347ec68095d691a0e8

// lib includes
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

// local includes
#include "config.h"
#include "confighttp.h"
#include "globals.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "stream.h"
#include "upnp.h"
#include "upnp_mapping.h"
#include "utility.h"

using namespace std::literals;

namespace upnp {

  /**
   * @brief UPnP port mapping description and lease state.
   */
  struct mapping_t {
    struct {
      std::string wan;  ///< External gateway port exposed by the mapping.
      std::string lan;  ///< Internal Sunshine port targeted by the mapping.
      std::string proto;  ///< Transport protocol used by the mapping.
    } port;  ///< WAN/LAN/protocol tuple for the mapped port.

    std::string description;  ///< Human-readable UPnP lease description advertised to the gateway.
  };

  /**
   * @brief A discovered IGD and the data needed to manage its port mappings.
   */
  struct igd_t {
    std::string host;  ///< Address of the IGD, taken from its description URL.
    urls_t urls;  ///< Control URLs of the IGD.
    IGDdatas data {};  ///< Service data of the IGD.
  };

  /**
   * @brief Describe the discovered IGDs other than the one already in the list.
   * @param device Device list from upnpDiscover().
   * @param igds List holding the IGD chosen by UPNP_GetValidIGD(); the other IGDs are appended to it.
   */
  static void add_other_igds(const device_t &device, std::vector<igd_t> &igds) {
    std::set<std::string, std::less<>> seen {igds.front().urls->rootdescURL};
    for (auto dev = device.get(); dev != nullptr; dev = dev->pNext) {
      if (!seen.emplace(dev->descURL).second) {
        continue;
      }

      igd_t igd;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr {};
      if (UPNP_GetIGDFromUrl(dev->descURL, &igd.urls.el, &igd.data, lan_addr.data(), (int) lan_addr.size()) != 1 || igd.urls->controlURL == nullptr || igd.urls->controlURL[0] == 0 || igd.data.first.servicetype[0] == 0) {
        continue;
      }

      igd.host = url_host(dev->descURL);
      igds.emplace_back(std::move(igd));
    }
  }

  /**
   * @brief Get the external address an IGD reports.
   * @param igd The IGD to ask.
   * @return The external address, or an empty string if the IGD doesn't report one.
   */
  static std::string external_address(const igd_t &igd) {
    std::array<char, INET6_ADDRESS_STRLEN> addr {};
    if (UPNP_GetExternalIPAddress(igd.urls->controlURL, igd.data.first.servicetype, addr.data()) != UPNPCOMMAND_SUCCESS) {
      return {};
    }

    return addr.data();
  }

  static std::string_view status_string(igd_status_e status) {
    using enum igd_status_e;

    switch (status) {
      case none:
        return "No IGD device found"sv;
      case connected:
        return "Valid IGD device found"sv;
#if (MINIUPNPC_API_VERSION >= 18)
      case private_ip:
        return "Valid IGD device found, but it has a private IP address"sv;
#endif
      case disconnected:
        return "Valid IGD device found, but it isn't connected"sv;
      case unknown_device:
        return "A UPnP device has been found, but it wasn't recognized as an IGD"sv;
    }

    return "Unknown status"sv;
  }

  /**
   * This function is a wrapper around UPNP_GetValidIGD() that returns the status code. There is a pre-processor
   * check to determine which version of the function to call based on the version of the MiniUPnPc library.
   */
  igd_status_e UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr) {
#if (MINIUPNPC_API_VERSION >= 18)
    auto status = UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size(), nullptr, 0);
#else
    auto status = UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size());
#endif
    return static_cast<igd_status_e>(status);
  }

  bool is_valid_igd(igd_status_e status) {
    using enum igd_status_e;

    switch (status) {
      case connected:
#if (MINIUPNPC_API_VERSION >= 18)
      case private_ip:
#endif
      case disconnected:
        return true;
      default:
        return false;
    }
  }

  /**
   * @brief Check whether a value is a private (LAN) IPv4 address.
   * @param address The value to check.
   * @return `true` if `address` is an IPv4 address in a private, shared or link-local range.
   */
  static bool is_private_ipv4(std::string_view address) {
    boost::system::error_code ec;
    const auto addr = boost::asio::ip::make_address_v4(std::string {address}, ec);
    return !ec && net::from_address(addr.to_string()) == net::LAN;
  }

  int shared_prefix_length(std::string_view a, std::string_view b) {
    boost::system::error_code ec_a;
    boost::system::error_code ec_b;
    const auto addr_a = boost::asio::ip::make_address_v4(std::string {a}, ec_a);
    const auto addr_b = boost::asio::ip::make_address_v4(std::string {b}, ec_b);
    if (ec_a || ec_b) {
      return -1;
    }

    return std::countl_zero(addr_a.to_uint() ^ addr_b.to_uint());
  }

  std::string url_host(std::string_view url) {
    if (const auto scheme = url.find("://"sv); scheme != std::string_view::npos) {
      url.remove_prefix(scheme + 3);
    }

    if (url.starts_with('[')) {
      return std::string {url.substr(1, url.find(']') - 1)};
    }

    return std::string {url.substr(0, url.find_first_of(":/"sv))};
  }

  std::size_t select_inner_igd(const std::vector<std::string> &igd_hosts, std::string_view lan_addr) {
    std::size_t best = 0;
    int best_bits = -1;
    for (std::size_t i = 0; i < igd_hosts.size(); ++i) {
      if (const auto bits = shared_prefix_length(igd_hosts[i], lan_addr); bits > best_bits) {
        best = i;
        best_bits = bits;
      }
    }

    return best;
  }

  std::optional<std::size_t> select_outer_igd(const std::vector<std::string> &igd_hosts, std::size_t inner, std::string_view inner_external_addr) {
    // A public external address means the inner gateway is the edge router: there is nothing in front of it to map.
    if (!is_private_ipv4(inner_external_addr)) {
      return std::nullopt;
    }

    // The outer router sits in the same network as the inner gateway's external address.
    constexpr int min_shared_bits = 16;
    std::optional<std::size_t> best;
    int best_bits = min_shared_bits - 1;
    for (std::size_t i = 0; i < igd_hosts.size(); ++i) {
      if (i == inner) {
        continue;
      }

      if (const auto bits = shared_prefix_length(igd_hosts[i], inner_external_addr); bits > best_bits) {
        best = i;
        best_bits = bits;
      }
    }

    return best;
  }

  /**
   * @brief Gateway calls through MiniUPnPc.
   */
  struct miniupnpc_gateway_t {
    /**
     * @brief Look up the rule on the requested gateway port.
     * @param rule Rule whose gateway, service, port and protocol are looked up.
     * @return The gateway's current rule.
     */
    mapping_entry_t lookup(const owned_mapping_t &rule) const {
      std::array<char, 16> client {};
      std::array<char, 6> port {};
      std::array<char, 80> description {};
      std::array<char, 4> enabled {};
      std::array<char, 16> lease {};
      const auto result = UPNP_GetSpecificPortMappingEntry(rule.control_url.c_str(), rule.service.c_str(), rule.external_port.c_str(), rule.protocol.c_str(), nullptr, client.data(), port.data(), description.data(), enabled.data(), lease.data());
      return {result, client.data(), port.data(), description.data(), enabled.data(), lease.data()};
    }

    /**
     * @brief Add or renew a rule.
     * @param rule Rule to add.
     * @param lease Requested lease in seconds, `0` for a rule without expiry.
     * @return MiniUPnPc result.
     */
    int add(const owned_mapping_t &rule, const std::string &lease) const {
      return UPNP_AddPortMapping(rule.control_url.c_str(), rule.service.c_str(), rule.external_port.c_str(), rule.internal_port.c_str(), rule.target.c_str(), rule.description.c_str(), rule.protocol.c_str(), nullptr, lease.c_str());
    }

    /**
     * @brief Remove a rule.
     * @param rule Rule to remove.
     * @return MiniUPnPc result.
     */
    int remove(const owned_mapping_t &rule) const {
      return UPNP_DeletePortMapping(rule.control_url.c_str(), rule.service.c_str(), rule.external_port.c_str(), rule.protocol.c_str(), nullptr);
    }
  };

  static_assert(mapping_gateway<miniupnpc_gateway_t>);

  /**
   * @brief Log a mapping attempt that left the port closed.
   * @param rule Requested rule.
   * @param outcome Mapping outcome.
   * @param error Last gateway result.
   */
  static void report_mapping(const owned_mapping_t &rule, map_outcome_e outcome, int error) {
    switch (outcome) {
      case map_outcome_e::conflict:
        BOOST_LOG(warning) << "UPnP port "sv << rule.external_port << " already forwards elsewhere; leaving it unchanged"sv;
        break;
      case map_outcome_e::disabled:
        BOOST_LOG(warning) << "UPnP port "sv << rule.external_port << " has a disabled manual mapping; leaving it unchanged"sv;
        break;
      case map_outcome_e::unknown:
        BOOST_LOG(warning) << "UPnP mapping ownership could not be checked: "sv << error;
        break;
      case map_outcome_e::failed:
        BOOST_LOG(warning) << "Failed to map "sv << rule.protocol << ' ' << rule.external_port << ": "sv << error;
        break;
      case map_outcome_e::mapped:
      case map_outcome_e::reused:
      case map_outcome_e::unrecorded:
      case map_outcome_e::cancelled:
        // Open ports need no message; recording failures are logged where they happen.
        break;
    }
  }

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t: public platf::deinit_t {
  public:
    deinit_t() {
      auto rtsp = std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT));
      auto video = std::to_string(net::map_port(stream::VIDEO_STREAM_PORT));
      auto audio = std::to_string(net::map_port(stream::AUDIO_STREAM_PORT));
      auto control = std::to_string(net::map_port(stream::CONTROL_PORT));
      auto gs_http = std::to_string(net::map_port(nvhttp::PORT_HTTP));
      auto gs_https = std::to_string(net::map_port(nvhttp::PORT_HTTPS));
      auto wm_http = std::to_string(net::map_port(confighttp::PORT_HTTPS));

      mappings.assign({
        {{rtsp, rtsp, "TCP"s}, "Sunshine - RTSP"s},
        {{video, video, "UDP"s}, "Sunshine - Video"s},
        {{audio, audio, "UDP"s}, "Sunshine - Audio"s},
        {{control, control, "UDP"s}, "Sunshine - Control"s},
        {{gs_http, gs_http, "TCP"s}, "Sunshine - Client HTTP"s},
        {{gs_https, gs_https, "TCP"s}, "Sunshine - Client HTTPS"s},
      });

      // Only map port for the Web Manager if it is configured to accept connection from WAN
      if (net::from_enum_string(config::nvhttp.origin_web_ui_allowed) > net::LAN) {
        mappings.emplace_back(mapping_t {{wm_http, wm_http, "TCP"s}, "Sunshine - Web UI"s});
      }

      // Start the mapping thread
      upnp_thread = std::jthread {&deinit_t::upnp_thread_proc, this};
    }

    /**
     * @brief Destroy the UPnP deinitializer.
     */
    ~deinit_t() {
      upnp_thread.join();
    }

    /**
     * @brief Opens pinholes for IPv6 traffic if the IGD is capable.
     * @details Not many IGDs support this feature, so we perform error logging with debug level.
     * @return `true` if the pinholes were opened successfully.
     */
    bool create_ipv6_pinholes() {
      int err;
      device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv6, 2, &err)};
      if (!device || err) {
        BOOST_LOG(debug) << "Couldn't discover any IPv6 UPNP devices"sv;
        return false;
      }

      IGDdatas data;
      urls_t urls;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr;
      auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
      if (!is_valid_igd(status)) {
        BOOST_LOG(debug) << "No valid IPv6 IGD: "sv << status_string(status);
        return false;
      }

      if (data.IPv6FC.controlurl[0] != 0) {
        int firewallEnabled;
        int pinholeAllowed;

        // Check if this firewall supports IPv6 pinholes
        err = UPNP_GetFirewallStatus(urls->controlURL_6FC, data.IPv6FC.servicetype, &firewallEnabled, &pinholeAllowed);
        if (err == UPNPCOMMAND_SUCCESS) {
          BOOST_LOG(debug) << "UPnP IPv6 firewall control available. Firewall is "sv
                           << (firewallEnabled ? "enabled"sv : "disabled"sv)
                           << ", pinhole is "sv
                           << (pinholeAllowed ? "allowed"sv : "disallowed"sv);

          if (pinholeAllowed) {
            // Create pinholes for each port
            auto mapping_period = std::to_string(PORT_MAPPING_LIFETIME.count());
            auto shutdown_event = mail::man->event<bool>(mail::shutdown);

            for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
              auto mapping = *it;
              std::array<char, 8> uniqueId;

              // Open a pinhole for the LAN port, since there will be no WAN->LAN port mapping on IPv6
              err = UPNP_AddPinhole(urls->controlURL_6FC, data.IPv6FC.servicetype, "", "0", lan_addr.data(), mapping.port.lan.c_str(), mapping.port.proto.c_str(), mapping_period.c_str(), uniqueId.data());
              if (err == UPNPCOMMAND_SUCCESS) {
                BOOST_LOG(debug) << "Successfully created pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan;
              } else {
                BOOST_LOG(debug) << "Failed to create pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
              }
            }

            return err == 0;
          } else {
            BOOST_LOG(debug) << "IPv6 pinholes are not allowed by the IGD"sv;
            return false;
          }
        } else {
          BOOST_LOG(debug) << "Failed to get IPv6 firewall status: "sv << err;
          return false;
        }
      } else {
        BOOST_LOG(debug) << "IPv6 Firewall Control is not supported by the IGD"sv;
        return false;
      }
    }

    /**
     * @brief Append recovery intent before creating a permanent rule, or completion after removing it.
     * @param mapping Rule to record.
     * @param added Whether this records creation intent.
     * @return Whether the complete event reached the file stream successfully.
     */
    bool journal_mapping(const owned_mapping_t &mapping, bool added) {
      const auto event = mapping_journal_event(mapping, added);
      if (event.empty()) {
        BOOST_LOG(warning) << "Invalid UPnP recovery identity; refusing an unrecorded permanent mapping"sv;
        return false;
      }
      std::ofstream out {journal_path(), std::ios::app};
      out << event;
      out.flush();
      out.close();
      if (!out) {
        BOOST_LOG(warning) << "Could not update UPnP recovery journal; refusing an unrecorded permanent mapping"sv;
        return false;
      }
      return true;
    }

    /**
     * @brief Remember a rule independently of discovery and persist permanent-rule intent before adding it.
     * @param mapping Rule owned by Sunshine.
     * @return Whether the rule can safely be managed.
     */
    bool remember_mapping(owned_mapping_t mapping) {
      auto found = std::find(owned_mappings.begin(), owned_mappings.end(), mapping);
      if (mapping.permanent && (found == owned_mappings.end() || !found->permanent) && !journal_mapping(mapping, true)) {
        return false;
      }
      if (found == owned_mappings.end()) {
        owned_mappings.push_back(std::move(mapping));
      } else {
        found->generation = generation;
        found->permanent = found->permanent || mapping.permanent;
      }
      return true;
    }

    /**
     * @brief Remove only recorded rules whose current destination, port and description still match.
     * @param stale_only Whether to retain rules wanted by the current discovery pass.
     */
    void unmap_owned_ports(bool stale_only = false) {
      const miniupnpc_gateway_t gateway {};
      std::erase_if(owned_mappings, [this, stale_only, &gateway](const owned_mapping_t &owned) {
        if (stale_only && owned.generation == generation) {
          return false;
        }
        const auto entry = gateway.lookup(owned);
        const auto state = classify_mapping(entry.result, entry.client, entry.port, entry.description, owned);
        if (state == mapping_state_e::unknown) {
          BOOST_LOG(warning) << "Could not check UPnP rule for cleanup: "sv << owned.control_url << ' ' << owned.external_port;
          return false;
        }
        if (state == mapping_state_e::ours) {
          const auto removed = gateway.remove(owned);
          if (removed != UPNPCOMMAND_SUCCESS && removed != 714) {
            BOOST_LOG(warning) << "Failed to remove UPnP rule: "sv << removed;
            return false;
          }
        }
        // Missing or replaced rules are forgotten without deleting the replacement.
        return !owned.permanent || journal_mapping(owned, false);
      });
      if (std::none_of(owned_mappings.begin(), owned_mappings.end(), [](const auto &mapping) {
            return mapping.permanent;
          })) {
        std::error_code ec;
        std::filesystem::remove(journal_path(), ec);
      }
    }

    /**
     * @brief Maps every port on an IGD, stopping early on shutdown.
     * @param igd The IGD to map the ports on.
     * @param addr Address the ports are forwarded to.
     * @return Whether every requested mapping succeeded before shutdown.
     */
    bool map_all_upnp_ports(const igd_t &igd, const std::string &addr) {
      std::vector<owned_mapping_t> rules;
      for (const auto &mapping : mappings) {
        owned_mapping_t rule {igd.urls->controlURL, igd.data.first.servicetype, addr, mapping.port.wan, mapping.port.lan, mapping.port.proto, mapping.description, false, generation};
        // A failed refresh must not forget a rule that was successfully added earlier.
        for (auto &owned : owned_mappings) {
          if (owned == rule) {
            owned.generation = generation;
          }
        }
        rules.push_back(std::move(rule));
      }

      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      const miniupnpc_gateway_t gateway {};
      return map_ports(
        gateway,
        rules,
        std::to_string(PORT_MAPPING_LIFETIME.count()),
        [this](const owned_mapping_t &rule) {
          return remember_mapping(rule);
        },
        [&shutdown_event]() {
          return shutdown_event->peek();
        },
        report_mapping
      );
    }

    /**
     * @brief Find the IGD of the network this host is attached to.
     * @details Behind two routers, the IGD chosen by UPNP_GetValidIGD() may be the outer one, which can't reach
     * this host. The other IGDs that answered discovery are described and the closest one is selected.
     * @param device Device list from upnpDiscover().
     * @param igds List holding the IGD chosen by UPNP_GetValidIGD(); the other IGDs are appended to it.
     * @param igd_hosts Receives the address of each IGD in `igds`.
     * @param lan_addr This host's local address.
     * @return Index of the selected IGD in `igds`.
     */
    static std::size_t find_gateway(const device_t &device, std::vector<igd_t> &igds, std::vector<std::string> &igd_hosts, const std::string &lan_addr) {
      igds.front().host = url_host(igds.front().urls->rootdescURL);
      add_other_igds(device, igds);
      for (const auto &igd : igds) {
        igd_hosts.push_back(igd.host);
      }

      const auto inner = select_inner_igd(igd_hosts, lan_addr);
      if (inner != 0) {
        BOOST_LOG(debug) << "Using the IGD on this host's network instead: "sv << igds[inner].urls->rootdescURL;
      }

      return inner;
    }

    /**
     * @brief Behind double NAT, map the ports on the IGD in front of the gateway.
     * @details If the gateway's external address is private, the same ports are mapped on the IGD in front of it,
     * forwarded to the gateway's external address. That IGD is kept to remove the mappings on shutdown.
     * @param igds The discovered IGDs.
     * @param igd_hosts Address of each IGD in `igds`.
     * @param inner Index of the gateway in `igds`.
     * @param first_pass Whether this is the first mapping pass since the mappings were last completed.
     */
    void map_outer_upnp_ports(std::vector<igd_t> &igds, const std::vector<std::string> &igd_hosts, std::size_t inner, bool first_pass) {
      const auto external_addr = external_address(igds[inner]);
      const auto outer = select_outer_igd(igd_hosts, inner, external_addr);
      if (!outer.has_value()) {
        // A missing reply is not proof that a working outer mapping is obsolete.
        for (auto &owned : owned_mappings) {
          if (external_addr.empty() || (is_private_ipv4(external_addr) && owned.target == external_addr)) {
            owned.generation = generation;
          }
        }
        if (is_private_ipv4(external_addr) && !warned_no_outer_igd) {
          BOOST_LOG(warning) << "The IGD's external address "sv << external_addr
                             << " is private and no UPnP IGD answered in front of it: ports must be forwarded on the outer router"sv;
          warned_no_outer_igd = true;
        }
        return;
      }

      auto &outer_igd = igds[*outer];
      const bool complete = map_all_upnp_ports(outer_igd, external_addr);
      if (complete && (first_pass || mapped_outer_host != outer_igd.host)) {
        BOOST_LOG(info) << "Completed UPnP port mappings to "sv << external_addr << " via "sv << outer_igd.urls->rootdescURL;
      }

      if (complete) {
        mapped_outer_host = outer_igd.host;
      }
      warned_no_outer_igd = false;
    }

    /**
     * @brief Maintains UPnP port forwarding rules
     */
    void upnp_thread_proc() {
      platf::set_thread_name("upnp");
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      std::ifstream journal {journal_path()};
      std::error_code journal_error;
      const bool journal_exists = std::filesystem::exists(journal_path(), journal_error);
      if (journal_error || (journal_exists && !journal.is_open())) {
        BOOST_LOG(error) << "Could not read UPnP recovery journal; leaving it intact and skipping new mappings"sv;
        return;
      }
      const std::string contents {(std::istreambuf_iterator<char>(journal)), std::istreambuf_iterator<char>()};
      if (journal.bad()) {
        BOOST_LOG(error) << "Could not finish reading UPnP recovery journal; leaving it intact"sv;
        return;
      }
      journal.close();
      owned_mappings = read_mapping_journal(contents);
      unmap_owned_ports();
      if (!config::sunshine.flags[config::flag::UPNP]) {
        return;
      }
      bool mapped = false;
      auto address_family = net::get_effective_address_family(net::af_from_enum_string(config::sunshine.address_family));

      // Refresh UPnP rules every few minutes. They can be lost if the router reboots,
      // WAN IP address changes, or various other conditions.
      do {
        int err = 0;
        device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv4, 2, &err)};
        if (!device || err) {
          BOOST_LOG(warning) << "Couldn't discover any IPv4 UPNP devices"sv;
          mapped = false;
          continue;
        }

        for (auto dev = device.get(); dev != nullptr; dev = dev->pNext) {
          BOOST_LOG(debug) << "Found device: "sv << dev->descURL;
        }

        std::array<char, INET6_ADDRESS_STRLEN> lan_addr;

        std::vector<igd_t> igds(1);
        auto status = upnp::UPNP_GetValidIGDStatus(device, &igds.front().urls, &igds.front().data, lan_addr);
        if (!is_valid_igd(status)) {
          BOOST_LOG(error) << status_string(status);
          mapped = false;
          continue;
        }

        std::string lan_addr_str {lan_addr.data()};

        BOOST_LOG(debug) << "Found valid IGD device: "sv << igds.front().urls->rootdescURL;

        std::vector<std::string> igd_hosts;
        const auto inner = find_gateway(device, igds, igd_hosts, lan_addr_str);
        ++generation;
        const bool complete = map_all_upnp_ports(igds[inner], lan_addr_str);

        if (!mapped && complete) {
          BOOST_LOG(info) << "Completed UPnP port mappings to "sv << lan_addr_str << " via "sv << igds[inner].urls->rootdescURL;
        }

        map_outer_upnp_ports(igds, igd_hosts, inner, !mapped);
        unmap_owned_ports(true);

        // If we are listening on IPv6 and the IGD has an IPv6 firewall enabled, try to create IPv6 firewall pinholes
        if (address_family == net::af_e::BOTH) {
          if (create_ipv6_pinholes() && !mapped) {
            // Only log the first time through
            BOOST_LOG(info) << "Successfully opened IPv6 pinholes on the IGD"sv;
          }
        }

        mapped = complete;
      } while (!shutdown_event->view(REFRESH_INTERVAL));

      // Discovery failures and gateway changes never discard ownership records.
      unmap_owned_ports();
    }

    /**
     * @brief Locate the append-only recovery journal used even when UPnP is disabled at the next start.
     * @return File containing outstanding permanent-rule intents.
     */
    static std::filesystem::path journal_path() {
      return platf::appdata() / "upnp_mappings.journal";
    }

    std::vector<mapping_t> mappings;  ///< Ports requested by Sunshine.
    std::vector<owned_mapping_t> owned_mappings;  ///< Rules retained independently of discovery results.
    std::string mapped_outer_host;  ///< Last outer gateway logged as mapped.
    std::size_t generation = 0;  ///< Current successful discovery pass.
    bool warned_no_outer_igd = false;  ///< Whether the missing outer IGD has already been reported.
    std::jthread upnp_thread;  ///< Worker thread that refreshes mappings until shutdown.
  };

  /**
   * @brief Start UPnP port mapping and return its shutdown guard.
   */
  std::unique_ptr<platf::deinit_t> start() {
    std::error_code ec;
    if (!config::sunshine.flags[config::flag::UPNP] && !std::filesystem::exists(platf::appdata() / "upnp_mappings.journal", ec)) {
      return nullptr;
    }

    return std::make_unique<deinit_t>();
  }
}  // namespace upnp
