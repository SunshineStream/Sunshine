// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

// Keyboard/mouse extension, independently negotiated from the gamepad protocol.
namespace lvg::km {
inline constexpr std::uint16_t k_abi_version = 1;
inline constexpr std::size_t k_keyboard_bytes = 29; // Keyboard page usages 0..0xE7.
inline constexpr std::uint8_t k_keyboard_report_id = 1;
inline constexpr std::uint8_t k_relative_report_id = 2;
inline constexpr std::uint8_t k_absolute_report_id = 3;
inline constexpr std::uint8_t k_consumer_report_id = 4;
enum capability : std::uint32_t {
  keyboard = 1u << 0,
  relative_mouse = 1u << 1,
  wheel_detents = 1u << 2,
  // Requires a native-verified binding to exactly one absolute coordinate
  // domain. It does not imply MOUSE_VIRTUAL_DESKTOP or display calibration.
  bound_absolute_mouse = 1u << 3,
  // The absolute pointer is mapped to the Windows primary display, not the
  // virtual desktop. Hosts must transform from their desktop coordinate space.
  primary_absolute_mouse = 1u << 4,
  consumer = 1u << 5,
};
inline constexpr std::uint32_t k_required_capabilities = keyboard | relative_mouse | wheel_detents | bound_absolute_mouse;

#pragma pack(push, 1)
struct header {
  std::uint32_t size;
  std::uint16_t version;
  std::uint16_t reserved;
};
struct query_response {
  header prefix;
  std::uint32_t capabilities;
  std::uint32_t queue_capacity;
  std::uint32_t absolute_target_generation; // Zero means no verified binding.
  std::uint32_t reserved;
};
struct keyboard_request {
  header prefix;
  std::uint16_t usage; // USB HID page 0x07; never a Win32 VK or UTF character.
  std::uint8_t pressed;
  std::uint8_t reserved;
};
// USB HID Consumer page usages for volume and playback keys.
struct consumer_request {
  header prefix;
  std::uint16_t usage;
  std::uint8_t pressed;
  std::uint8_t reserved;
};
struct button_request {
  header prefix;
  std::uint8_t button; // HID order: left=1, right=2, middle=3, back=4, forward=5.
  std::uint8_t pressed;
  std::uint16_t reserved;
};
struct relative_request {
  header prefix;
  std::int16_t x; // Positive right/down, raw relative counts, not pixels.
  std::int16_t y;
  std::int8_t wheel; // Whole detents: positive up/right. Not Win32 120ths.
  std::int8_t pan;
  std::uint16_t reserved;
};
struct absolute_request {
  header prefix;
  std::uint16_t x; // 0..65535 in the verified bound coordinate domain.
  std::uint16_t y;
  std::uint32_t target_generation;
};
#pragma pack(pop)
static_assert(sizeof(header) == 8);
static_assert(sizeof(query_response) == 24);
static_assert(sizeof(keyboard_request) == 12);
static_assert(sizeof(consumer_request) == 12);
static_assert(sizeof(button_request) == 12);
static_assert(sizeof(relative_request) == 16);
static_assert(sizeof(absolute_request) == 16);

template<class T> constexpr T request() noexcept {
  T result {};
  result.prefix = {sizeof(T), k_abi_version, 0};
  return result;
}
template<class T> constexpr bool valid(const T &value) noexcept {
  return value.prefix.size == sizeof(T) && value.prefix.version == k_abi_version && value.prefix.reserved == 0;
}
} // namespace lvg::km
