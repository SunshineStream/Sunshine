/**
 * @file src/server/upnp.h
 * @brief Declarations for UPnP port mapping.
 */
#pragma once

#include <miniupnpc/miniupnpc.h>

#include "src/platform/common.h"

/**
 * @brief UPnP port mapping.
 */
namespace upnp {
  constexpr auto INET6_ADDRESS_STRLEN = 46;
  constexpr auto IPv4 = 0;
  constexpr auto IPv6 = 1;
  constexpr auto PORT_MAPPING_LIFETIME = 3600s;
  constexpr auto REFRESH_INTERVAL = 120s;

  using device_t = util::safe_ptr<UPNPDev, freeUPNPDevlist>;

  KITTY_USING_MOVE_T(urls_t, UPNPUrls, , {
    FreeUPNPUrls(&el);
  });

  /**
   * @brief Get the valid IGD status.
   * @param device The device.
   * @param urls The URLs.
   * @param data The IGD data.
   * @param lan_addr The LAN address.
   * @return The UPnP Status.
   * @retval 0 No IGD found.
   * @retval 1 A valid connected IGD has been found.
   * @retval 2 A valid IGD has been found but it reported as not connected.
   * @retval 3 An UPnP device has been found but was not recognized as an IGD.
   */
  int
  UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr);

  [[nodiscard]] std::unique_ptr<platf::deinit_t>
  start();
}  // namespace upnp
