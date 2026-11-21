// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// The control-device ABI shared by Vibeshine and the UMDF VHF source driver.
// Keep every request fixed-size and METHOD_BUFFERED. Do not add pointers,
// handles, strings, or compiler-sized fields to this wire contract.

#pragma once

#include <cstddef>
#include <cstdint>

#include <windows.h>
#include <winioctl.h>

namespace lvg {

// Version 2 widened the feedback payload: a DualSense adaptive-trigger update
// is 23 bytes and could not be expressed in version 1's 16-byte payload, and
// splitting it across events would lose data to the driver's single pending
// feedback slot. A version mismatch is rejected rather than reinterpreted.
inline constexpr std::uint16_t k_protocol_version = 2;
inline constexpr std::uint32_t k_max_controllers = 16;

// This is the source device's private control interface, never the HID child
// interface. Both the Vibeshine client and driver compile this same value.
inline constexpr GUID k_device_interface_guid {
  0x27debbf5,
  0x1d1e,
  0x4e9c,
  {0x90, 0x6d, 0xd1, 0x04, 0xb1, 0x41, 0x8b, 0x2b}
};

inline constexpr wchar_t k_root_hardware_id[] = L"ROOT\\VIBESHINEVIRTUALGAMEPAD";

enum class profile : std::uint16_t {
  // Reserved for protocol compatibility. The public driver refuses this value
  // until the project has an accepted VID/PID allocation.
  generic_hid = 1,
  xbox_360 = 2,
  xbox_one = 3,
  xbox_series = 4,
  dualshock_4 = 5,
  dualsense = 6,
  switch_pro = 7,
  // Reserved for protocol compatibility. Its report encoder remains available
  // for development, but the public driver refuses it pending an accepted
  // VID/PID allocation.
  generic_pid = 8,
  // Valve Steam Controller (2026, wired). HID-native like the PlayStation pads:
  // Steam opens the vendor collection itself and drives its feature-report
  // control channel, so the identity travels with the real report contract.
  steam_controller = 9,
};

enum button_mask : std::uint32_t {
  // These values are deliberately identical to Vibeshine's existing Windows
  // platform contract. Keeping the normalized ABI stable makes the adapter a
  // field copy rather than a brittle semantic remapping.
  dpad_up = 0x000001u,
  dpad_down = 0x000002u,
  dpad_left = 0x000004u,
  dpad_right = 0x000008u,
  start = 0x000010u,
  back = 0x000020u,
  left_stick = 0x000040u,
  right_stick = 0x000080u,
  left_shoulder = 0x000100u,
  right_shoulder = 0x000200u,
  home = 0x000400u,
  south = 0x001000u,
  east = 0x002000u,
  west = 0x004000u,
  north = 0x008000u,
  paddle_1 = 0x010000u,
  paddle_2 = 0x020000u,
  paddle_3 = 0x040000u,
  paddle_4 = 0x080000u,
  touchpad = 0x100000u,
  misc = 0x200000u,
  // Moonlight extension (LI_CCAP_GRIP_SENSE): capacitive grip touch of the Steam
  // Controller (2026), not buttons. Ignored by every other profile.
  left_grip_touch = 0x400000u,
  right_grip_touch = 0x800000u,
  // Moonlight extension (LI_CCAP_STICK_TOUCH): capacitive touch of the Steam
  // Controller (2026) sticks, held rather than pressed. Without them the driver
  // derives stick touch from deflection. Ignored by every other profile.
  left_stick_touch = 0x1000000u,
  right_stick_touch = 0x2000000u,
  left_pad_click = 0x04000000u,
  right_pad_click = 0x08000000u,
  left_trigger_click = 0x10000000u,
  right_trigger_click = 0x20000000u,
  // Host-owned metadata, not a client button. Set on each input only when the
  // SC2026 client advertised authoritative clicks and capacitive touch states.
  sc26_buttons_explicit = 0x40000000u,
};

enum class feedback_type : std::uint16_t {
  none = 0,
  generic_rumble_rgb = 1,
  raw_output_report = 2,
  // Rumble without a light. The payload is still generic_rumble_rgb_feedback,
  // but its colour channels carry no meaning: a DirectInput PID effect says
  // nothing about an LED, and reporting one would make a client turn a real
  // controller's light off every time a game started a force.
  generic_rumble = 3,
  // All four Xbox actuators in one event. The driver keeps a single pending
  // feedback slot per controller, so body and trigger rumble have to travel
  // together or one of them would be dropped by coalescing.
  xbox_rumble = 4,
  // A PlayStation output report: rumble, lightbar, and (DualSense only) the
  // adaptive trigger programs, player LEDs, and microphone LED. One report
  // carries all of it, so one event does too.
  playstation_output = 5,
  // A Steam Controller (2026) haptic output report exactly as the host wrote
  // it (steam_haptic_feedback): 0x80 rumble, 0x81 pulse, 0x82 command, 0x83
  // LFO, 0x84 sweep or 0x85 script. Steam's UI drives the pads with 0x81 and
  // 0x82, games with 0x80. A client with the real pads replays the report; one
  // without renders rumble from it.
  steam_haptic = 6,
  // Steam's control-channel settings for a Steam Controller (2026), exactly as
  // the host wrote them (steam_settings_feedback): SET_SETTINGS_VALUES 0x87 as
  // (id, u16) triples, at most nine per event (a longer write becomes several
  // events, in order), or LOAD_DEFAULT_SETTINGS 0x8E with no triples. The
  // virtual device keeps answering GET_SETTINGS from its own table; a client
  // with the real controller forwards the write so the firmware runs with
  // Steam's settings (haptic gain and intensity among them), not its defaults.
  steam_settings = 7,
};

// DualSense adaptive trigger modes, as the console's output report encodes them.
enum class trigger_effect_mode : std::uint8_t {
  off = 0x00,
  feedback = 0x01,
  weapon = 0x02,
  vibration = 0x06,
};

using profile_mask_t = std::uint32_t;

[[nodiscard]] constexpr profile_mask_t profile_bit(const profile value) noexcept {
  const auto index = static_cast<std::uint16_t>(value);
  return index == 0 || index > 32 ? 0u : (1u << (index - 1));
}

enum feature_mask : std::uint32_t {
  feature_input_state = 1u << 0,
  feature_feedback = 1u << 1,
  feature_touch = 1u << 2,
  feature_motion = 1u << 3,
  feature_battery = 1u << 4,
  feature_hid_feature_reports = 1u << 5,
};

enum class raw_hid_operation : std::uint8_t {
  write_report = 1,
  set_feature = 2,
  get_feature = 3,
};

#pragma pack(push, 1)

struct request_header {
  std::uint32_t size;
  std::uint16_t version;
  std::uint16_t reserved;
};

struct create_controller_request {
  request_header header;
  std::uint32_t controller_id;
  profile requested_profile;
  std::uint16_t reserved;
};

struct query_info_request {
  request_header header;
};

// Feedback the driver had to discard for one controller, so a host can tell a
// quiet controller from one whose output is being thrown away. Each counter is
// monotonic for the life of the slot (destroy resets it) and saturates at 255
// on the wire; the driver keeps the full count. Steam's haptic and settings
// rings and the input pump drop the oldest entry when the host stops polling.
struct feedback_counters {
  // Ordinary (non-stop) Steam haptic events evicted from the per-slot ring to
  // admit a newer one. A stop that replaced an older equivalent stop is a
  // coalescing, not an eviction, and is not counted.
  std::uint8_t haptic_evicted;
  // Settings events (one chunk of a SET_SETTINGS_VALUES write, or a
  // LOAD_DEFAULT_SETTINGS) evicted from the settings ring.
  std::uint8_t settings_evicted;
  // Input transitions (button, D-pad, trigger-threshold, touch lifecycle)
  // evicted from the report pump's 16-deep ordered ring to admit a newer
  // transition because VHF had no read pending for too long. Asides (the
  // 0x43 battery level, the 0x44 haptic ack) share that ring but never
  // count: a new aside that finds the ring full is dropped instead of
  // evicting a transition, and an aside a transition evicts is regenerated
  // by the next stop pulse or battery tick.
  std::uint8_t transitions_dropped;
  // Steam Controller feature commands the driver acknowledged without
  // understanding (sc26_usb::feature_state::unknown_commands).
  std::uint8_t unknown_commands;
};

[[nodiscard]] constexpr std::uint8_t saturate_counter(const std::uint32_t value) noexcept {
  return value > 0xFFu ? std::uint8_t {0xFF} : static_cast<std::uint8_t>(value);
}

// ioctl_query_info answers two request shapes without a version bump; the
// header's size field is what tells them apart:
//   * query_info_request (8 bytes): the device-wide answer, `counters` all
//     zero. This is the handshake every host sends, and hosts built before
//     the counters existed reject a response whose word 24..27 is non-zero,
//     so the device query must keep it zero.
//   * controller_id_request (12 bytes): the same answer with `counters`
//     filled for that slot (STATUS_NOT_FOUND if empty, STATUS_ACCESS_DENIED
//     if another handle owns it). A driver that predates the counters answers
//     this shape with STATUS_INVALID_BUFFER_SIZE, which the client reports as
//     ERROR_REVISION_MISMATCH.
// `counters` occupies the word that was `reserved` before; the layout and
// size are unchanged.
struct query_info_response {
  request_header header;
  std::uint16_t minimum_protocol_version;
  std::uint16_t maximum_protocol_version;
  profile_mask_t available_profiles;
  std::uint32_t available_features;
  std::uint32_t maximum_controllers;
  feedback_counters counters;
};

struct controller_id_request {
  request_header header;
  std::uint32_t controller_id;
};

// Axes are Vibeshine's normalized state: sticks are signed with **positive
// meaning up and right**, and triggers are 0..255. Each profile converts to its
// own device's convention, because they disagree - HID sticks are positive-down
// and a DualShock 4's are unsigned. A client must not pre-convert; doing so on
// top of the profile's conversion inverts the vertical axes.
struct input_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint32_t buttons;
  std::int16_t left_x;
  std::int16_t left_y;
  std::int16_t right_x;
  std::int16_t right_y;
  std::uint8_t left_trigger;
  std::uint8_t right_trigger;
  std::uint16_t reserved;
};

// Touch lifecycle, matching Vibeshine's normalized touch events.
enum class touch_event : std::uint8_t {
  hover = 0,
  down = 1,
  up = 2,
  move = 3,
  cancel = 4,
  cancel_all = 5,
};

enum class motion_kind : std::uint8_t {
  accelerometer = 1,
  gyroscope = 2,
};

enum class battery_state : std::uint8_t {
  unknown = 0,
  not_present = 1,
  discharging = 2,
  charging = 3,
  full = 4,
  not_charging = 5,
};

// Coordinates and pressure are normalized to 0..65535 by the client adapter.
// contact_index is a profile-independent logical contact, not a platform
// pointer identifier.
struct touch_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint8_t contact_index;
  std::uint8_t event_type;
  std::uint16_t x;
  std::uint16_t y;
  std::uint16_t pressure;
  std::uint16_t reserved;
};

// Motion uses signed milli-units: m/s^2 * 1000 for accelerometers and degrees
// per second * 1000 for gyroscopes. The profile implementation owns any
// device-specific scaling and calibration.
struct motion_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint8_t motion_type;
  std::uint8_t reserved0[3];
  std::int32_t x_milli;
  std::int32_t y_milli;
  std::int32_t z_milli;
};

struct battery_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint8_t percent;
  std::uint8_t flags;
  std::uint16_t reserved;
};

// Which fields of a steam_controller_attributes_request the host read from the
// real controller. A field whose bit is clear keeps the slot's default
// (sc26_usb.h: the captured unit's build times and board revision, the
// placeholder build id, the synthetic per-slot serials). The bit values are
// the client's (Moonlight LI_SCATTR_*); 0x10 and 0x20 are its radio and dongle
// build times, which the wired reply has no tag for, so the driver ignores them.
enum steam_controller_attribute_flags : std::uint32_t {
  sc_attr_firmware_build_time = 0x01,
  sc_attr_bootloader_build_time = 0x02,
  sc_attr_board_revision = 0x04,
  sc_attr_capabilities = 0x08,
  sc_attr_build_id = 0x40,
  sc_attr_serials = 0x80,
};

// The real controller's firmware attributes for a Steam Controller slot, so the
// virtual device answers GET_ATTRIBUTES_VALUES / GET_DEVICE_INFO with them and
// Steam judges the real firmware, not the captured constants. Strings are
// ASCII, NUL padded; the driver keeps a string only when it is non-empty and
// printable (0x20..0x7E) up to its first NUL, and terminates it at [15].
// Applied in place: nothing goes on the wire until Steam asks again.
struct steam_controller_attributes_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint32_t flags;  // steam_controller_attribute_flags
  std::uint32_t firmware_build_time;
  std::uint32_t bootloader_build_time;
  std::uint32_t board_revision;
  std::uint32_t capabilities;
  char build_id[16];
  char unit_serial[16];
  char board_serial[16];
};

struct generic_rumble_rgb_feedback {
  std::uint16_t low_frequency;
  std::uint16_t high_frequency;
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t reserved;
};

struct xbox_rumble_feedback {
  std::uint16_t low_frequency;
  std::uint16_t high_frequency;
  std::uint16_t left_trigger;
  std::uint16_t right_trigger;
};

// Mirrors Vibeshine's adaptive-trigger feedback message so the adapter is a
// field copy. `parameters` is the raw effect program for that trigger.
struct trigger_effect_feedback {
  std::uint8_t mode;
  std::uint8_t parameters[10];
};

struct playstation_output_feedback {
  std::uint16_t low_frequency;
  std::uint16_t high_frequency;
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  // Bit 0 lightbar valid, bit 1 triggers valid, bit 2 player LEDs valid,
  // bit 3 microphone LED valid. A DualShock 4 only ever sets the first.
  std::uint8_t valid;
  std::uint8_t player_leds;
  std::uint8_t microphone_led;
  trigger_effect_feedback left_trigger;
  trigger_effect_feedback right_trigger;
};

inline constexpr std::uint8_t ps_output_lightbar_valid = 0x01;
inline constexpr std::uint8_t ps_output_triggers_valid = 0x02;
inline constexpr std::uint8_t ps_output_player_leds_valid = 0x04;
inline constexpr std::uint8_t ps_output_microphone_led_valid = 0x08;

// The report id first, then the payload the descriptor declares for it; at most
// 10 bytes on the wire (sc26_usb::report_size).
struct steam_haptic_feedback {
  std::uint8_t length;  // bytes valid in report, id included
  std::uint8_t reserved[3];
  std::uint8_t report[12];
};

// A control-channel settings write as Steam sent it: command 0x87
// (SET_SETTINGS_VALUES) with `count` (id, lo, hi) triples, or 0x8E
// (LOAD_DEFAULT_SETTINGS) with count 0. A 64-byte report holds twenty triples;
// the driver splits a write into events of at most k_steam_settings_triples_max.
inline constexpr std::uint8_t k_steam_settings_triples_max = 9;
struct steam_settings_feedback {
  std::uint8_t command;
  std::uint8_t count;
  std::uint8_t reserved[3];
  std::uint8_t triples[k_steam_settings_triples_max * 3];
};

struct raw_hid_report_feedback {
  raw_hid_operation operation;
  std::uint8_t report_id;
  std::uint8_t data_size;
  std::uint8_t reserved;
  std::uint8_t data[12];
};

struct feedback_event {
  request_header header;
  std::uint32_t controller_id;
  feedback_type type;
  std::uint16_t payload_size;
  std::uint8_t payload[32];
};

// The payload each feedback type carries. A client rejects an event whose
// payload_size disagrees, and any type it does not know (the driver only ever
// writes whole structs, so a consumer may memcpy by type after that check).
inline constexpr std::uint16_t k_feedback_payload_unknown = 0xFFFF;

[[nodiscard]] constexpr std::uint16_t feedback_payload_size(const feedback_type type) noexcept {
  switch (type) {
    case feedback_type::none:
      return 0;
    case feedback_type::generic_rumble_rgb:
    case feedback_type::generic_rumble:
      return sizeof(generic_rumble_rgb_feedback);
    case feedback_type::raw_output_report:
      return sizeof(raw_hid_report_feedback);
    case feedback_type::xbox_rumble:
      return sizeof(xbox_rumble_feedback);
    case feedback_type::playstation_output:
      return sizeof(playstation_output_feedback);
    case feedback_type::steam_haptic:
      return sizeof(steam_haptic_feedback);
    case feedback_type::steam_settings:
      return sizeof(steam_settings_feedback);
  }
  return k_feedback_payload_unknown;
}

#pragma pack(pop)

static_assert(sizeof(request_header) == 8);
static_assert(sizeof(create_controller_request) == 16);
static_assert(sizeof(query_info_request) == 8);
static_assert(sizeof(feedback_counters) == 4);
static_assert(sizeof(query_info_response) == 28);
static_assert(offsetof(query_info_response, counters) == 24);
static_assert(sizeof(controller_id_request) == 12);
static_assert(sizeof(input_state_request) == 28);
static_assert(sizeof(touch_state_request) == 22);
static_assert(sizeof(motion_state_request) == 28);
static_assert(sizeof(battery_state_request) == 16);
static_assert(sizeof(steam_controller_attributes_request) == 80);
static_assert(offsetof(steam_controller_attributes_request, build_id) == 32);
static_assert(sizeof(generic_rumble_rgb_feedback) == 8);
static_assert(sizeof(xbox_rumble_feedback) == 8);
static_assert(sizeof(raw_hid_report_feedback) == 16);
static_assert(sizeof(trigger_effect_feedback) == 11);
static_assert(sizeof(playstation_output_feedback) == 32);
static_assert(sizeof(steam_haptic_feedback) == 16);
static_assert(sizeof(steam_settings_feedback) == 32);
static_assert(sizeof(steam_settings_feedback) <= sizeof(feedback_event::payload));
// Exactly fills the payload; anything larger needs another version bump.
static_assert(sizeof(playstation_output_feedback) <= sizeof(feedback_event::payload));
static_assert(sizeof(feedback_event) == 48);

// Controllers do not survive the source device leaving D0 (sleep, hibernate,
// shutdown): the driver retires its VHF children during D0 exit. Each slot stays owned by its handle, and
// input/feedback requests then fail with ERROR_DEVICE_REMOVED until the owner
// destroys it (which succeeds) or creates it again. No wire format changes, so
// the protocol version does not either.
inline constexpr DWORD ioctl_query_info =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_create_controller =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_destroy_controller =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_input_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_poll_feedback =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_touch_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_motion_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_battery_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
// Additive, no protocol version bump (like the controller shape of
// ioctl_query_info): a driver built before it fails the request with
// STATUS_INVALID_DEVICE_REQUEST, which the client reports as
// ERROR_REVISION_MISMATCH, and the virtual device keeps its defaults.
inline constexpr DWORD ioctl_set_steam_controller_attributes =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);

template<class request_t>
[[nodiscard]] inline bool valid_request(const request_t *request, const std::size_t bytes) {
  return request != nullptr && bytes == sizeof(request_t) &&
         request->header.size == sizeof(request_t) &&
         request->header.version == k_protocol_version &&
         request->header.reserved == 0;
}

}  // namespace lvg
