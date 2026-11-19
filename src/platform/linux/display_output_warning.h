/**
 * @file src/platform/linux/display_output_warning.h
 * @brief Detect legacy numeric output settings that fall back to another display.
 */
#pragma once

// standard includes
#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace platf {
  /**
   * @brief Determine whether a KMS or wlroots output setting needs migration guidance.
   * @param names Display names returned by the selected capture backend.
   * @param requested Configured display name or legacy number.
   * @return True when a numeric setting cannot match any enumerated output.
   */
  inline bool needs_output_name_migration_warning(std::span<const std::string> names, std::string_view requested) {
    return !names.empty() && !requested.empty() &&
           std::ranges::all_of(requested, [](char ch) {
             return ch >= '0' && ch <= '9';
           }) &&
           std::ranges::find(names, requested) == names.end();
  }
}  // namespace platf
