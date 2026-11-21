// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// The USB device contract shared by our UHID and VHF implementations. Report
// IDs, usage pages, usages and lengths identify the transport to native hosts;
// they are not interchangeable padding even when the byte counts match.
namespace lvg::ds4_usb {
inline constexpr char report_descriptor[] =
      "\x05\x01\x09\x05\xa1\x01\x85\x01\x09\x30\x09\x31\x09\x32\x09\x35\x15\x00\x26\xff\x00\x75\x08\x95"
      "\x04\x81\x02\x09\x39\x15\x00\x25\x07\x35\x00\x46\x3b\x01\x65\x14\x75\x04\x95\x01\x81\x42\x65\x00"
      "\x05\x09\x19\x01\x29\x0e\x15\x00\x25\x01\x75\x01\x95\x0e\x81\x02\x06\x00\xff\x09\x20\x75\x06\x95"
      "\x01\x15\x00\x25\x3f\x81\x02\x05\x01\x09\x33\x09\x34\x15\x00\x26\xff\x00\x75\x08\x95\x02\x81\x02"
      "\x06\x00\xff\x09\x21\x95\x36\x81\x02\x85\x05\x09\x22\x95\x1f\x91\x02\x85\x04\x09\x23\x95\x24\xb1"
      "\x02\x85\x02\x09\x24\x95\x24\xb1\x02\x85\x08\x09\x25\x95\x03\xb1\x02\x85\x10\x09\x26\x95\x04\xb1"
      "\x02\x85\x11\x09\x27\x95\x02\xb1\x02\x85\x12\x06\x02\xff\x09\x21\x95\x0f\xb1\x02\x85\x13\x09\x22"
      "\x95\x16\xb1\x02\x85\x14\x06\x05\xff\x09\x20\x95\x10\xb1\x02\x85\x15\x09\x21\x95\x2c\xb1\x02\x06"
      "\x80\xff\x85\x80\x09\x20\x95\x06\xb1\x02\x85\x81\x09\x21\x95\x06\xb1\x02\x85\x82\x09\x22\x95\x05"
      "\xb1\x02\x85\x83\x09\x23\x95\x01\xb1\x02\x85\x84\x09\x24\x95\x04\xb1\x02\x85\x85\x09\x25\x95\x06"
      "\xb1\x02\x85\x86\x09\x26\x95\x06\xb1\x02\x85\x87\x09\x27\x95\x23\xb1\x02\x85\x88\x09\x28\x95\x3f"
      "\xb1\x02\x85\x89\x09\x29\x95\x02\xb1\x02\x85\x90\x09\x30\x95\x05\xb1\x02\x85\x91\x09\x31\x95\x03"
      "\xb1\x02\x85\x92\x09\x32\x95\x03\xb1\x02\x85\x93\x09\x33\x95\x0c\xb1\x02\x85\x94\x09\x34\x95\x3f"
      "\xb1\x02\x85\xa0\x09\x40\x95\x06\xb1\x02\x85\xa1\x09\x41\x95\x01\xb1\x02\x85\xa2\x09\x42\x95\x01"
      "\xb1\x02\x85\xa3\x09\x43\x95\x30\xb1\x02\x85\xa4\x09\x44\x95\x0d\xb1\x02\x85\xf0\x09\x47\x95\x3f"
      "\xb1\x02\x85\xf1\x09\x48\x95\x3f\xb1\x02\x85\xf2\x09\x49\x95\x0f\xb1\x02\x85\xa7\x09\x4a\x95\x01"
      "\xb1\x02\x85\xa8\x09\x4b\x95\x01\xb1\x02\x85\xa9\x09\x4c\x95\x08\xb1\x02\x85\xaa\x09\x4e\x95\x01"
      "\xb1\x02\x85\xab\x09\x4f\x95\x39\xb1\x02\x85\xac\x09\x50\x95\x39\xb1\x02\x85\xad\x09\x51\x95\x0b"
      "\xb1\x02\x85\xae\x09\x52\x95\x01\xb1\x02\x85\xaf\x09\x53\x95\x02\xb1\x02\x85\xb0\x09\x54\x95\x3f"
      "\xb1\x02\x85\xe0\x09\x57\x95\x02\xb1\x02\x85\xb3\x09\x55\x95\x3f\xb1\x02\x85\xb4\x09\x55\x95\x3f"
      "\xb1\x02\x85\xb5\x09\x56\x95\x3f\xb1\x02\x85\xd0\x09\x58\x95\x3f\xb1\x02\x85\xd4\x09\x59\x95\x3f"
      "\xb1\x02\xc0";


inline constexpr std::size_t report_descriptor_size = sizeof(report_descriptor) - 1;
inline constexpr std::uint8_t calibration_id = 0x02;
inline constexpr std::uint8_t pairing_id = 0x12;
inline constexpr std::uint8_t control_id = 0x14;
inline constexpr std::uint8_t firmware_id = 0xa3;

// Zero sensor bias. +/-8192 gyro counts at +/-512 degrees/s gives 16
// counts/(degree/s); +/-8192 accelerometer counts is +/-1g. USB interleaves
// each gyro axis's positive and negative endpoints (Bluetooth does not).
inline constexpr std::array<std::uint8_t, 37> calibration = {
  0x02, 0, 0, 0, 0, 0, 0,
  0x00, 0x20, 0x00, 0xe0,
  0x00, 0x20, 0x00, 0xe0,
  0x00, 0x20, 0x00, 0xe0,
  0x00, 0x02, 0x00, 0x02,
  0x00, 0x20, 0x00, 0xe0,
  0x00, 0x20, 0x00, 0xe0,
  0x00, 0x20, 0x00, 0xe0,
  0, 0
};

// USB protocol revision, separate from our driver package version. Native
// consumers read the little-endian word at offset 35 and require >=0x3100.
// Build date/time remain empty: this is our implementation, not factory data.
inline constexpr std::array<std::uint8_t, 49> firmware = [] {
  std::array<std::uint8_t, 49> value {};
  value[0] = firmware_id;
  value[34] = 0x01;
  value[36] = 0x43;
  value[37] = 0x03;
  value[41] = 0x51;
  value[43] = 0x05;
  value[46] = 0x80;
  value[47] = 0x03;
  return value;
}();

struct feature_state {
  // Pairing-report byte order, least-significant address octet first.
  std::array<std::uint8_t, 6> address {0x44, 0x41, 0x50, 0x47, 0x56, 0x02};
  bool sensors_enabled = true;
};

// libScePad overlays ReportFeatureInMacAll: client MAC, then 0x08 0x25 0x00,
// then host MAC. Dying Light's libScePad 1.0.4.1 only reverses the MAC, but
// later Sony PC libraries read those three bytes as part of the struct.
inline void fill_pairing(std::uint8_t *buffer, const std::array<std::uint8_t, 6> &address) noexcept {
  buffer[0] = pairing_id;
  std::memcpy(buffer + 1, address.data(), address.size());
  buffer[7] = 0x08;
  buffer[8] = 0x25;
  buffer[9] = 0x00;
}

// Return the actual wire length, not the caller's maximum HID feature size.
// Clear any successful response's tail: VHF may reuse a larger report buffer.
inline std::size_t get_feature(std::uint8_t id, std::uint8_t *buffer,
                               std::size_t capacity, const feature_state &state) noexcept {
  std::size_t length = 0;
  switch (id) {
    case calibration_id: length = calibration.size(); break;
    case pairing_id: length = 16; break;
    case control_id: length = 17; break;
    case firmware_id: length = firmware.size(); break;
    default: return 0;
  }
  if (buffer == nullptr || capacity < length) return 0;
  std::memset(buffer, 0, capacity);
  buffer[0] = id;
  switch (id) {
    case calibration_id: std::memcpy(buffer, calibration.data(), length); break;
    case firmware_id: std::memcpy(buffer, firmware.data(), length); break;
    case pairing_id: fill_pairing(buffer, state.address); break;
    case control_id: buffer[1] = state.sensors_enabled ? 0x02 : 0; break;
  }
  return length;
}

// Native USB hosts enable sensor reporting with feature 0x14, command 0x02.
// Our USB reports already carry sensors. Record this supported command and
// reject other feature writes instead of acknowledging unimplemented commands.
inline bool set_feature(std::uint8_t id, const std::uint8_t *buffer,
                        std::size_t size, feature_state &state) noexcept {
  if (id != control_id || buffer == nullptr || size < 17 ||
      buffer[0] != id || buffer[1] != 0x02) return false;
  for (std::size_t i = 2; i < 17; ++i) {
    if (buffer[i] != 0) return false;
  }
  state.sensors_enabled = true;
  return true;
}
}  // namespace lvg::ds4_usb
