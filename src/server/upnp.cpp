/**
 * @file src/server/upnp.cpp
 * @brief Definitions for UPnP port mapping.
 */
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/server/confighttp.h"
#include "src/server/network.h"
#include "src/server/nvhttp.h"
#include "src/server/rtsp.h"
#include "src/server/upnp.h"
#include "src/stream.h"
#include "src/utility.h"

using namespace std::literals;

namespace upnp {

  struct mapping_t {
    struct {
      std::string wan;
      std::string lan;
      std::string proto;
    } port;

    std::string description;
  };

  static std::string_view
  status_string(int status) {
    switch (status) {
      case 0:
        return "No IGD device found"sv;
      case 1:
        return "Valid IGD device found"sv;
      case 2:
        return "Valid IGD device found,  but it isn't connected"sv;
      case 3:
        return "A UPnP device has been found,  but it wasn't recognized as an IGD"sv;
    }

    return "Unknown status"sv;
  }

  /**
   * This function is a wrapper around UPNP_GetValidIGD() that returns the status code. There is a pre-processor
   * check to determine which version of the function to call based on the version of the MiniUPnPc library.
   */
  int
  UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr) {
#if (MINIUPNPC_API_VERSION >= 18)
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), lan_addr.size(), nullptr, 0);
#else
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), lan_addr.size());
#endif
  }

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
        { { rtsp, rtsp, "TCP"s }, "Sunshine - RTSP"s },
        { { video, video, "UDP"s }, "Sunshine - Video"s },
        { { audio, audio, "UDP"s }, "Sunshine - Audio"s },
        { { control, control, "UDP"s }, "Sunshine - Control"s },
        { { gs_http, gs_http, "TCP"s }, "Sunshine - Client HTTP"s },
        { { gs_https, gs_https, "TCP"s }, "Sunshine - Client HTTPS"s },
      });

      // Only map port for the Web Manager if it is configured to accept connection from WAN
      if (net::from_enum_string(config::nvhttp.origin_web_ui_allowed) > net::LAN) {
        mappings.emplace_back(mapping_t { { wm_http, wm_http, "TCP"s }, "Sunshine - Web UI"s });
      }

      // Start the mapping thread
      upnp_thread = std::thread { &deinit_t::upnp_thread_proc, this };
    }

    ~deinit_t() {
      upnp_thread.join();
    }

    /**
     * @brief Opens pinholes for IPv6 traffic if the IGD is capable.
     * @details Not many IGDs support this feature, so we perform error logging with debug level.
     * @return `true` if the pinholes were opened successfully.
     */
    bool
    create_ipv6_pinholes() {
      int err;
      device_t device { upnpDiscover(2000, nullptr, nullptr, 0, IPv6, 2, &err) };
      if (!device || err) {
        BOOST_LOG(debug) << "Couldn't discover any IPv6 UPNP devices"sv;
        return false;
      }

      IGDdatas data;
      urls_t urls;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr;
      auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
      if (status != 1 && status != 2) {
        BOOST_LOG(debug) << "No valid IPv6 IGD: "sv << status_string(status);
        return false;
      }

      if (data.IPv6FC.controlurl[0] != 0) {
        int firewallEnabled, pinholeAllowed;

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
              char uniqueId[8];

              // Open a pinhole for the LAN port, since there will be no WAN->LAN port mapping on IPv6
              err = UPNP_AddPinhole(urls->controlURL_6FC,
                data.IPv6FC.servicetype,
                "", "0",
                lan_addr.data(),
                mapping.port.lan.c_str(),
                mapping.port.proto.c_str(),
                mapping_period.c_str(),
                uniqueId);
              if (err == UPNPCOMMAND_SUCCESS) {
                BOOST_LOG(debug) << "Successfully created pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan;
              }
              else {
                BOOST_LOG(debug) << "Failed to create pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
              }
            }

            return err == 0;
          }
          else {
            BOOST_LOG(debug) << "IPv6 pinholes are not allowed by the IGD"sv;
            return false;
          }
        }
        else {
          BOOST_LOG(debug) << "Failed to get IPv6 firewall status: "sv << err;
          return false;
        }
      }
      else {
        BOOST_LOG(debug) << "IPv6 Firewall Control is not supported by the IGD"sv;
        return false;
      }
    }

    /**
     * @brief Maps a port via UPnP.
     * @param data IGDdatas from UPNP_GetValidIGD()
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param lan_addr Local IP address to map to
     * @param mapping Information about port to map
     * @return `true` on success.
     */
    bool
    map_upnp_port(const IGDdatas &data, const urls_t &urls, const std::string &lan_addr, const mapping_t &mapping) {
      char intClient[16];
      char intPort[6];
      char desc[80];
      char enabled[4];
      char leaseDuration[16];
      bool indefinite = false;

      // First check if this port is already mapped successfully
      BOOST_LOG(debug) << "Checking for existing UPnP port mapping for "sv << mapping.port.wan;
      auto err = UPNP_GetSpecificPortMappingEntry(
        urls->controlURL,
        data.first.servicetype,
        // In params
        mapping.port.wan.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        // Out params
        intClient, intPort, desc, enabled, leaseDuration);
      if (err == 714) {  // NoSuchEntryInArray
        BOOST_LOG(debug) << "Mapping entry not found for "sv << mapping.port.wan;
      }
      else if (err == UPNPCOMMAND_SUCCESS) {
        // Some routers change the description, so we can't check that here
        if (!std::strcmp(intClient, lan_addr.c_str())) {
          if (std::atoi(leaseDuration) == 0) {
            BOOST_LOG(debug) << "Static mapping entry found for "sv << mapping.port.wan;

            // It's a static mapping, so we're done here
            return true;
          }
          else {
            BOOST_LOG(debug) << "Mapping entry found for "sv << mapping.port.wan << " ("sv << leaseDuration << " seconds remaining)"sv;
          }
        }
        else {
          BOOST_LOG(warning) << "UPnP conflict detected with: "sv << intClient;

          // Some UPnP IGDs won't let unauthenticated clients delete other conflicting port mappings
          // for security reasons, but we will give it a try anyway.
          err = UPNP_DeletePortMapping(
            urls->controlURL,
            data.first.servicetype,
            mapping.port.wan.c_str(),
            mapping.port.proto.c_str(),
            nullptr);
          if (err) {
            BOOST_LOG(error) << "Unable to delete conflicting UPnP port mapping: "sv << err;
            return false;
          }
        }
      }
      else {
        BOOST_LOG(error) << "UPNP_GetSpecificPortMappingEntry() failed: "sv << err;

        // If we get a strange error from the router, we'll assume it's some old broken IGDv1
        // device and only use indefinite lease durations to hopefully avoid confusing it.
        if (err != 606) {  // Unauthorized
          indefinite = true;
        }
      }

      // Add/update the port mapping
      auto mapping_period = std::to_string(indefinite ? 0 : PORT_MAPPING_LIFETIME.count());
      err = UPNP_AddPortMapping(
        urls->controlURL,
        data.first.servicetype,
        mapping.port.wan.c_str(),
        mapping.port.lan.c_str(),
        lan_addr.data(),
        mapping.description.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        mapping_period.c_str());

      if (err != UPNPCOMMAND_SUCCESS && !indefinite) {
        // This may be an old/broken IGD that doesn't like non-static mappings.
        BOOST_LOG(debug) << "Trying static mapping after failure: "sv << err;
        err = UPNP_AddPortMapping(
          urls->controlURL,
          data.first.servicetype,
          mapping.port.wan.c_str(),
          mapping.port.lan.c_str(),
          lan_addr.data(),
          mapping.description.c_str(),
          mapping.port.proto.c_str(),
          nullptr,
          "0");
      }

      if (err) {
        BOOST_LOG(error) << "Failed to map "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
        return false;
      }

      BOOST_LOG(debug) << "Successfully mapped "sv << mapping.port.proto << ' ' << mapping.port.lan;
      return true;
    }

    /**
     * @brief Unmaps all ports.
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param data IGDdatas from UPNP_GetValidIGD()
     */
    void
    unmap_all_upnp_ports(const urls_t &urls, const IGDdatas &data) {
      for (auto it = std::begin(mappings); it != std::end(mappings); ++it) {
        auto status = UPNP_DeletePortMapping(
          urls->controlURL,
          data.first.servicetype,
          it->port.wan.c_str(),
          it->port.proto.c_str(),
          nullptr);

        if (status && status != 714) {  // NoSuchEntryInArray
          BOOST_LOG(warning) << "Failed to unmap "sv << it->port.proto << ' ' << it->port.lan << ": "sv << status;
        }
        else {
          BOOST_LOG(debug) << "Successfully unmapped "sv << it->port.proto << ' ' << it->port.lan;
        }
      }
    }

    /**
     * @brief Maintains UPnP port forwarding rules
     */
    void
    upnp_thread_proc() {
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      bool mapped = false;
      IGDdatas data;
      urls_t mapped_urls;
      auto address_family = net::af_from_enum_string(config::sunshine.address_family);

      // Refresh UPnP rules every few minutes. They can be lost if the router reboots,
      // WAN IP address changes, or various other conditions.
      do {
        int err = 0;
        device_t device { upnpDiscover(2000, nullptr, nullptr, 0, IPv4, 2, &err) };
        if (!device || err) {
          BOOST_LOG(warning) << "Couldn't discover any IPv4 UPNP devices"sv;
          mapped = false;
          continue;
        }

        for (auto dev = device.get(); dev != nullptr; dev = dev->pNext) {
          BOOST_LOG(debug) << "Found device: "sv << dev->descURL;
        }

        std::array<char, INET6_ADDRESS_STRLEN> lan_addr;

        urls_t urls;
        auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
        if (status != 1 && status != 2) {
          BOOST_LOG(error) << status_string(status);
          mapped = false;
          continue;
        }

        std::string lan_addr_str { lan_addr.data() };

        BOOST_LOG(debug) << "Found valid IGD device: "sv << urls->rootdescURL;

        for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
          map_upnp_port(data, urls, lan_addr_str, *it);
        }

        if (!mapped) {
          BOOST_LOG(info) << "Completed UPnP port mappings to "sv << lan_addr_str << " via "sv << urls->rootdescURL;
        }

        // If we are listening on IPv6 and the IGD has an IPv6 firewall enabled, try to create IPv6 firewall pinholes
        if (address_family == net::af_e::BOTH) {
          if (create_ipv6_pinholes() && !mapped) {
            // Only log the first time through
            BOOST_LOG(info) << "Successfully opened IPv6 pinholes on the IGD"sv;
          }
        }

        mapped = true;
        mapped_urls = std::move(urls);
      } while (!shutdown_event->view(REFRESH_INTERVAL));

      if (mapped) {
        // Unmap ports upon termination
        BOOST_LOG(info) << "Unmapping UPNP ports..."sv;
        unmap_all_upnp_ports(mapped_urls, data);
      }
    }

    std::vector<mapping_t> mappings;
    std::thread upnp_thread;
  };

  std::unique_ptr<platf::deinit_t>
  start() {
    if (!config::sunshine.flags[config::flag::UPNP]) {
      return nullptr;
    }

    return std::make_unique<deinit_t>();
  }
}  // namespace upnp
