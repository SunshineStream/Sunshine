/**
 * @file src/platform/linux/display_output_selection.h
 * @brief Helpers for resolving legacy numeric Linux capture output names.
 */
#pragma once

// standard includes
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace platf::display_output {
  /**
   * @brief Parse a zero-based capture index without accepting partial matches.
   * @param name Configured output name or numeric index.
   * @return The index, or no value for a connector name or invalid index.
   */
  inline std::optional<std::size_t> parse_output_index(std::string_view name) {
    if (name.empty()) {
      return std::nullopt;
    }

    std::size_t index;
    const auto end = name.data() + name.size();
    const auto result = std::from_chars(name.data(), end, index);
    if (result.ec != std::errc {} || result.ptr != end) {
      return std::nullopt;
    }
    return index;
  }

  /**
   * @brief Record the position of the requested capture index in the usable display list.
   * @param requested Configured numeric capture index, if any.
   * @param capture_index Current plane's capture index, including skipped planes.
   * @param display_position Current usable display's position, excluding skipped planes.
   * @param selected The selected list position to update on a match.
   */
  inline void match_output_index(
    std::optional<std::size_t> requested,
    std::size_t capture_index,
    std::size_t display_position,
    std::optional<std::size_t> &selected
  ) {
    if (requested == capture_index) {
      selected = display_position;
    }
  }

  /**
   * @brief Resolve a legacy index into the Wayland registry's enumerated display list.
   * @param names Output names in the order reported by the compositor.
   * @param requested Configured output name or zero-based index.
   * @return The preferred list position for a valid numeric alias, or no override.
   */
  inline std::optional<std::size_t> wl_output_index(
    std::span<const std::string> names,
    std::string_view requested
  ) {
    if (std::ranges::find(names, requested) != names.end()) {
      return std::nullopt;  // Let shared selection match the exact name first.
    }
    const auto index = parse_output_index(requested);
    return index && *index < names.size() ? index : std::nullopt;
  }
}  // namespace platf::display_output
