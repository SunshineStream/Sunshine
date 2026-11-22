/**
 * @file src/platform/linux/kms_display.h
 * @brief Display enumeration data for one KMS capture backend.
 */
#pragma once

// standard includes
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "display_geometry.h"

namespace platf::kms {
  /**
   * @brief Connector identity and geometry for one KMS monitor.
   */
  struct monitor_t {
    std::uint32_t type;  ///< DRM connector type.
    std::uint32_t index;  ///< Index within the connector type.
    std::uint32_t monitor_index;  ///< Index in the capture backend's display list.
    touch_port_t viewport;  ///< Monitor geometry for capture and absolute input.
  };

  /**
   * @brief DRM card and its monitor descriptors indexed by CRTC ID.
   */
  struct card_descriptor_t {
    std::string path;  ///< DRM card device path.
    std::map<std::uint32_t, monitor_t> crtc_to_monitor;  ///< Monitors associated with the card.
  };

  /**
   * @brief A display discovered on a particular card and plane.
   */
  struct output_t {
    std::string name;  ///< DRM connector name.
    std::size_t card_index;  ///< Index into this enumeration's card descriptors.
    std::uint32_t plane_id;  ///< ID of the DRM plane that was scanning out this output.
    std::uint32_t crtc_id;  ///< CRTC the plane was attached to during enumeration.
  };

  /**
   * @brief Display names, card descriptors, and desktop geometry enumerated together for one memory type.
   */
  struct display_enumeration_t {
    std::vector<output_t> outputs;  ///< Displays in capture order.
    std::vector<card_descriptor_t> cards;  ///< DRM cards in capture order.
    desktop_geometry_t desktop;  ///< Geometry of this enumeration's available outputs.

    /**
     * @brief Map a connector name or a legacy numeric display name to a capture index.
     *
     * @param name Display name from the stream or host configuration.
     * @return Display index, zero for the default name, or no value for an unknown connector.
     */
    std::optional<std::size_t> display_index(std::string_view name) const {
      if (name.empty()) {
        return 0;
      }

      std::size_t index;
      const auto [end, parse_error] = std::from_chars(name.data(), name.data() + name.size(), index);
      if (end == name.data() + name.size() && parse_error == std::errc {}) {
        return index;
      }

      auto position = std::ranges::find(outputs, name, &output_t::name);
      if (position == outputs.end()) {
        return std::nullopt;
      }
      return static_cast<std::size_t>(position - outputs.begin());
    }
  };
}  // namespace platf::kms
