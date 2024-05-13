/**
 * @file src/display_device/dd.h
 * @brief todo this file may not exist in the future,
 * todo        and we only import <libdisplaydevice/display_device.h> instead, avoiding duplication
 */

#pragma once

#include <bitset>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace dd {

  enum class device_state_e {
    inactive,
    active,
    primary /**< Primary state is also implicitly active. */
  };

  /**
   * @brief The device's HDR state in the operating system.
   */
  enum class hdr_state_e {
    unknown, /**< HDR state could not be retrieved from the OS (even if the display supports it). */
    disabled,
    enabled
  };

  /**
  * @brief Display's origin position.
  * @note Display origin may vary given the compositor running.
  */
  struct origin_t {
    int x;
    int y;

    [[nodiscard]] bool is_primary() const {
      return this->x == 0 && this->y == 0;
    }
    // For JSON serialization
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(origin_t, x, y)
  };

  struct resolution_t {
    unsigned int width;
    unsigned int height;
    double scale_factor;

    // For JSON serialization
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(resolution_t, width, height, scale_factor)
  };

  /**
   * @brief Display's refresh rate.
   * @note Floating point is stored in a "numerator/denominator" form.
   */
  struct refresh_rate_t {
    unsigned int numerator;
    unsigned int denominator;

    // For JSON serialization
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(refresh_rate_t, numerator, denominator)
  };

  /**
   * @brief Display's mode (resolution + refresh rate).
   * @see resolution_t
   * @see refresh_rate_t
   */
  struct mode_t {
    resolution_t resolution;
    refresh_rate_t refresh_rate;

    // For JSON serialization
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(mode_t, resolution, refresh_rate)
  };


  namespace options {
    struct current_settings_t {
      origin_t origin;
      mode_t mode;

      [[nodiscard]] bool is_primary() const {
        return origin.is_primary();
      }

      // For JSON serialization
      NLOHMANN_DEFINE_TYPE_INTRUSIVE(current_settings_t, origin, mode)
    };

    struct info_t {
      std::string id;
      std::string friendly_name;
      current_settings_t current_settings;

      // For JSON serialization
      NLOHMANN_DEFINE_TYPE_INTRUSIVE(info_t, id, friendly_name, current_settings)
    };
  }
}