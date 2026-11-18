/**
 * @file src/platform/linux/display_geometry.h
 * @brief Desktop bounds for absolute input across Linux capture backends.
 */
#pragma once

// standard includes
#include <algorithm>

// local includes
#include "src/platform/common.h"

namespace platf {
  /**
   * @brief Accumulate monitor extents while preserving a negative desktop origin.
   *
   * Output offsets are compositor coordinates. Logical dimensions define the
   * absolute input desktop; physical dimensions retain capture's pixel extents.
   */
  class desktop_geometry_t {
  public:
    /**
     * @brief Include an active monitor in the desktop bounds.
     *
     * @param viewport Monitor position and physical/logical dimensions.
     */
    void add(const touch_port_t &viewport) {
      if (viewport.width <= 0 || viewport.height <= 0) {
        return;
      }
      const bool has_logical_size = viewport.logical_width > 0 && viewport.logical_height > 0;
      const int logical_width = has_logical_size ? viewport.logical_width : viewport.width;
      const int logical_height = has_logical_size ? viewport.logical_height : viewport.height;
      if (!initialized_) {
        bounds_ = viewport;
        bounds_.logical_width = logical_width;
        bounds_.logical_height = logical_height;
        bounds_.output_name.clear();
        initialized_ = true;
        return;
      }
      const int left = std::min(bounds_.offset_x, viewport.offset_x);
      const int top = std::min(bounds_.offset_y, viewport.offset_y);
      bounds_.width = std::max(bounds_.offset_x + bounds_.width, viewport.offset_x + viewport.width) - left;
      bounds_.height = std::max(bounds_.offset_y + bounds_.height, viewport.offset_y + viewport.height) - top;
      bounds_.logical_width = std::max(bounds_.offset_x + bounds_.logical_width, viewport.offset_x + logical_width) - left;
      bounds_.logical_height = std::max(bounds_.offset_y + bounds_.logical_height, viewport.offset_y + logical_height) - top;
      bounds_.offset_x = left;
      bounds_.offset_y = top;
    }

    /**
     * @brief Get the union of included monitors.
     *
     * @return Desktop origin and extents, or zero dimensions if no valid monitor was included.
     */
    const touch_port_t &bounds() const {
      return bounds_;
    }

    /**
     * @brief Publish the accumulated input desktop on a capture display.
     *
     * @param display Capture display receiving physical and logical desktop geometry.
     */
    void apply(display_t &display) const {
      display.env_offset_x = bounds_.offset_x;
      display.env_offset_y = bounds_.offset_y;
      display.env_width = bounds_.width;
      display.env_height = bounds_.height;
      display.env_logical_width = bounds_.logical_width;
      display.env_logical_height = bounds_.logical_height;
    }

  private:
    touch_port_t bounds_ {};  ///< Union of all valid monitor extents.
    bool initialized_ = false;  ///< Whether a valid monitor has established the origin.
  };
}  // namespace platf
