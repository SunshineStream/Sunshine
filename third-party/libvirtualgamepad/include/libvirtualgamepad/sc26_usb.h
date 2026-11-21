// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// Steam Controller (2026, Valve "Ibex") wired USB contract. Portable: no
// Windows headers, so the encoders and the feature-report responder can be unit
// tested on any platform. The driver adapts Vibeshine's protocol structs onto
// the plain integer API here.
//
// Sources (all public, none from Valve firmware or Steam binaries):
//   - Linux drivers/hid/hid-steam.c (Ibex report ids, sizes, byte offsets, the
//     feature-report command set and reply framing)
//   - SDL src/joystick/hidapi/SDL_hidapi_steam_triton.c and Valve's
//     controller_structs.h / controller_constants.h (button bits, state layout,
//     haptic output reports, settings/attribute ids, sensor and axis scaling)
//   - Linux drivers/hid/hid-ids.h and SDL usb_ids.h (vendor and product ids)
//   - The author's own wired unit, captured with USBPcap on 2026-10-05
//     (captures/: report descriptor, bcdDevice, attribute values, the
//     feature-report conversation Steam holds with it, report sizes/rates)
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lvg::sc26_usb {

inline constexpr std::uint16_t vendor_id = 0x28DE;
inline constexpr std::uint16_t product_id = 0x1302;   // Wired 2026 controller.
inline constexpr std::uint16_t product_id_ble = 0x1303;
// bcdDevice of the author's unit (USB device descriptor, captures/sc26-plugin-summary.txt;
// Windows shows it as REV_0307 in captures/steam-controller-hid.txt).
inline constexpr std::uint16_t version = 0x0307;

// Report ids, as the real descriptor declares them (captures/sc26-report-descriptor-1.hex).
// The lizard-mode mouse (0x40) and keyboard (0x41) live in their own top-level
// collections; everything else is in the vendor collection Steam opens.
inline constexpr std::uint8_t mouse_report_id = 0x40;     // 6 bytes, lizard mode; all-zero when off.
inline constexpr std::uint8_t keyboard_report_id = 0x41;  // 9 bytes, lizard mode; all-zero when off.
inline constexpr std::uint8_t features_report_id = 0x01;  // Control channel.
inline constexpr std::uint8_t features_report_id_2 = 0x02; // Second 64-byte feature report; Steam never touched it.
inline constexpr std::uint8_t input_report_id = 0x42;     // Wired state (with quaternion), ~250 Hz.
inline constexpr std::uint8_t battery_report_id = 0x43;   // Every ~3.5 s on the real unit.
inline constexpr std::uint8_t haptic_ack_report_id = 0x44; // 6 bytes; answers an all-zero 0x81 (haptic stop), one per side; other haptic reports get none.
inline constexpr std::uint8_t input_report_id_ble = 0x45; // BLE state, no quaternion (46 bytes).
inline constexpr std::uint8_t wireless_report_id = 0x79;  // 2 bytes.
inline constexpr std::uint8_t report_id_7b = 0x7B;        // 13 bytes; not seen on the wire.
inline constexpr std::uint8_t haptic_rumble_id = 0x80;
inline constexpr std::uint8_t haptic_pulse_id = 0x81;
inline constexpr std::uint8_t haptic_command_id = 0x82;
inline constexpr std::uint8_t haptic_lfo_id = 0x83;
inline constexpr std::uint8_t haptic_sweep_id = 0x84;
inline constexpr std::uint8_t haptic_script_id = 0x85;
inline constexpr std::uint8_t output_report_id_86 = 0x86;  // 4 bytes; purpose unknown, not seen.
inline constexpr std::uint8_t output_report_id_87 = 0x87;  // 64 bytes; purpose unknown, not seen.
inline constexpr std::uint8_t output_report_id_88 = 0x88;  // 64 bytes; purpose unknown, not seen.
inline constexpr std::uint8_t output_report_id_89 = 0x89;  // 64 bytes; purpose unknown, not seen.
inline constexpr std::uint8_t output_report_first_id = haptic_rumble_id;
inline constexpr std::uint8_t output_report_last_id = output_report_id_89;

inline constexpr std::size_t input_report_size = 54;    // hid-steam: REPORT_ID_INPUT size 54
inline constexpr std::size_t battery_report_size = 15;  // hid-steam: REPORT_ID_BATTERY size 15
inline constexpr std::size_t feature_report_size = 64;  // id + 63 bytes (HID_FEATURE_REPORT_BYTES)
inline constexpr std::size_t haptic_report_size = 10;   // 0x80 rumble: id + 9 (HID_RUMBLE_OUTPUT_REPORT_BYTES)
inline constexpr std::size_t haptic_pulse_report_size = 8;    // 0x81, as the descriptor declares it
inline constexpr std::size_t haptic_command_report_size = 4;  // 0x82

// Wire size (including the id) of every report the real descriptor declares; 0 for
// ids it does not. The driver accepts output reports of any of these ids.
[[nodiscard]] constexpr std::size_t report_size(const std::uint8_t id) noexcept {
  switch (id) {
    case features_report_id: case features_report_id_2: return feature_report_size;
    case mouse_report_id: return 6;
    case keyboard_report_id: return 9;
    case input_report_id: return input_report_size;
    case battery_report_id: return battery_report_size;
    case haptic_ack_report_id: return 6;
    case input_report_id_ble: return 46;
    case wireless_report_id: return 2;
    case report_id_7b: return 13;
    case haptic_rumble_id: return haptic_report_size;
    case haptic_pulse_id: return haptic_pulse_report_size;
    case haptic_command_id: return haptic_command_report_size;
    case haptic_lfo_id: return 10;
    case haptic_sweep_id: return 9;
    case haptic_script_id: return 4;
    case output_report_id_86: return 4;
    case output_report_id_87: case output_report_id_88: case output_report_id_89: return 64;
    default: return 0;
  }
}
[[nodiscard]] constexpr bool is_output_report(const std::uint8_t id) noexcept {
  return id >= output_report_first_id && id <= output_report_last_id;
}

// Button bits of the u32 at offset 2 (SDL TritonButtons).
enum button : std::uint32_t {
  btn_a = 0x00000001u,
  btn_b = 0x00000002u,
  btn_x = 0x00000004u,
  btn_y = 0x00000008u,
  btn_qam = 0x00000010u,
  btn_r3 = 0x00000020u,
  btn_view = 0x00000040u,
  btn_r4 = 0x00000080u,
  btn_r5 = 0x00000100u,
  btn_r = 0x00000200u,
  btn_dpad_down = 0x00000400u,
  btn_dpad_right = 0x00000800u,
  btn_dpad_left = 0x00001000u,
  btn_dpad_up = 0x00002000u,
  btn_menu = 0x00004000u,
  btn_l3 = 0x00008000u,
  btn_steam = 0x00010000u,
  btn_l4 = 0x00020000u,
  btn_l5 = 0x00040000u,
  btn_l = 0x00080000u,
  btn_right_stick_touch = 0x00100000u,
  btn_right_pad_touch = 0x00200000u,
  btn_right_pad_click = 0x00400000u,
  btn_right_trigger_click = 0x00800000u,
  btn_left_stick_touch = 0x01000000u,
  btn_left_pad_touch = 0x02000000u,
  btn_left_pad_click = 0x04000000u,
  btn_left_trigger_click = 0x08000000u,
  btn_right_grip_touch = 0x10000000u,
  btn_left_grip_touch = 0x20000000u,
};

// Feature-report commands (controller_constants.h / hid-steam.c).
enum command : std::uint8_t {
  cmd_clear_digital_mappings = 0x81,
  cmd_get_attributes_values = 0x83,
  cmd_set_default_digital_mappings = 0x85,
  cmd_set_settings_values = 0x87,
  cmd_get_settings_values = 0x89,
  cmd_get_settings_maxs = 0x8B,
  cmd_get_settings_defaults = 0x8C,
  cmd_load_default_settings = 0x8E,
  cmd_trigger_haptic_pulse = 0x8F,
  cmd_get_string_attribute = 0xAE,
  // Seen in the Steam capture (captures/sc26-steam-handshake.txt) but in none of
  // the public sources. Steam writes the first three and never reads a reply;
  // it reads replies to the last two, so the responder answers them in the
  // shapes the real unit used.
  cmd_write_c1 = 0xC1,  // 16 bytes: ff ff ff ff 03 09 05 ff ff ff ff ff ff ff ff ff
  cmd_write_dc = 0xDC,  // 2 bytes: 01 02
  cmd_write_e2 = 0xE2,  // 2 bytes: 01 20
  cmd_get_keyed_value = 0xED,  // payload = ASCII key ("esb/bond", "esb/bond_2", "user/wireless_transport")
  cmd_get_device_info = 0xF2,  // payload = sub id 0..2; reply echoes it (see set_feature)
};

enum attribute : std::uint8_t {
  attr_unique_id = 0,
  attr_product_id = 1,
  attr_capabilities = 2,
  attr_firmware_version = 3,
  attr_firmware_build_time = 4,
  attr_radio_firmware_build_time = 5,
  attr_radio_device_id0 = 6,
  attr_radio_device_id1 = 7,
  attr_dongle_firmware_build_time = 8,
  attr_board_revision = 9,
  attr_bootloader_build_time = 10,
  attr_connection_interval_us = 11,
};
// GET_STRING_ATTRIBUTE request is [0xAE][0x15][tag]: 0x15 is the *length* Steam
// asks for (tag + 20 chars), not a tag. The capture shows tag 0 = board serial
// ("MXA..."), tag 1 = unit serial ("FXA...", the USB iSerialNumber); the reply
// is always 20 bytes: [tag][string, NUL padded].
inline constexpr std::uint8_t string_attr_board_serial = 0x00;
inline constexpr std::uint8_t string_attr_unit_serial = 0x01;
inline constexpr std::uint8_t string_attr_reply_length = 20;

inline constexpr std::uint8_t setting_lizard_mode = 9;
inline constexpr std::uint8_t setting_imu_mode = 48;

// What the unit reports after LOAD_DEFAULT_SETTINGS and for GET_SETTINGS_DEFAULTS:
// lizard mode on, everything else zero.
[[nodiscard]] constexpr std::uint16_t default_setting(const std::uint8_t id) noexcept {
  return id == setting_lizard_mode ? 1u : 0u;
}

inline constexpr std::uint8_t setting_count = 99;  // SETTING_COUNT in controller_constants.h

// The settings table of a fresh unit (and after LOAD_DEFAULT_SETTINGS).
[[nodiscard]] constexpr std::array<std::uint16_t, setting_count> default_settings() noexcept {
  std::array<std::uint16_t, setting_count> table {};
  for (std::size_t id = 0; id < setting_count; ++id) {
    table[id] = default_setting(static_cast<std::uint8_t>(id));
  }
  return table;
}

// Charge states of the battery report (EChargeState).
inline constexpr std::uint8_t charge_reset = 0;
inline constexpr std::uint8_t charge_discharging = 1;
inline constexpr std::uint8_t charge_charging = 2;
inline constexpr std::uint8_t charge_src_validate = 3;
inline constexpr std::uint8_t charge_done = 4;

// Sensor scaling (SDL): gyro full scale 2000 deg/s over +/-32768, accel 2 g.
inline constexpr std::int32_t gyro_counts_per_1000_dps = 16384;  // counts = dps*1000 * 16384 / 1000000
inline constexpr std::int32_t accel_counts_per_g = 16384;
inline constexpr std::int32_t milli_g = 9807;  // Earth gravity in mm/s^2.

#pragma pack(push, 1)

// TritonMTUFull_t behind the report id (controller_structs.h); offsets below
// match hid-steam's steam_ibex_*_mappings tables.
struct input_report {
  std::uint8_t report_id;
  std::uint8_t sequence;
  std::uint32_t buttons;
  std::int16_t left_trigger;   // 0..32767
  std::int16_t right_trigger;
  std::int16_t left_x;         // +/-32767, positive up/right
  std::int16_t left_y;
  std::int16_t right_x;
  std::int16_t right_y;
  std::int16_t left_pad_x;     // full int16 range (x reaches -32768 at the left edge), zero when untouched
  std::int16_t left_pad_y;
  std::uint16_t left_pressure; // 0..32767
  std::int16_t right_pad_x;
  std::int16_t right_pad_y;
  std::uint16_t right_pressure;
  std::uint32_t imu_timestamp;
  std::int16_t accel_x;
  std::int16_t accel_y;
  std::int16_t accel_z;
  std::int16_t gyro_x;
  std::int16_t gyro_y;
  std::int16_t gyro_z;
  std::int16_t quat_w;
  std::int16_t quat_x;
  std::int16_t quat_y;
  std::int16_t quat_z;
};

// TritonBatteryStatus_t behind the report id.
struct battery_report {
  std::uint8_t report_id;
  std::uint8_t charge_state;
  std::uint8_t battery_level;  // 0..100
  std::uint16_t battery_voltage_mv;
  std::uint16_t system_voltage_mv;
  std::uint16_t input_voltage_mv;
  std::uint16_t current_ma;
  std::uint16_t input_current_ma;
  std::uint16_t temperature;
};

// Control channel: [report id][type][length][payload].
struct feature_report {
  std::uint8_t report_id;
  std::uint8_t type;
  std::uint8_t length;
  std::uint8_t payload[feature_report_size - 3];
};

struct haptic_rumble_report {  // 0x80
  std::uint8_t report_id;
  std::uint8_t type;
  std::uint16_t intensity;
  std::uint16_t left_speed;
  std::int8_t left_gain;
  std::uint16_t right_speed;
  std::int8_t right_gain;
};

struct haptic_pulse_report {  // 0x81, 8 bytes. Steam's UI click: side, on 400 us, off 0, repeat 1.
  std::uint8_t report_id;
  std::uint8_t side;  // firmware: 1 = left, 0 = right
  std::uint16_t on_us;
  std::uint16_t off_us;
  std::uint16_t repeat;
};

struct haptic_command_report {  // 0x82, 4 bytes. Seen: [side][02][f2], [side][01][fd].
  std::uint8_t report_id;
  std::uint8_t side;
  std::uint8_t command;
  std::uint8_t argument;
};

#pragma pack(pop)

static_assert(sizeof(input_report) == input_report_size);
static_assert(sizeof(battery_report) == battery_report_size);
static_assert(sizeof(feature_report) == feature_report_size);
static_assert(sizeof(haptic_rumble_report) == haptic_report_size);
static_assert(sizeof(haptic_pulse_report) == haptic_pulse_report_size);
static_assert(sizeof(haptic_command_report) == haptic_command_report_size);
static_assert(report_size(haptic_rumble_id) == sizeof(haptic_rumble_report));
static_assert(report_size(haptic_pulse_id) == sizeof(haptic_pulse_report));
static_assert(report_size(haptic_command_id) == sizeof(haptic_command_report));
// Pinned to hid-steam's Ibex tables (byte index into the report incl. id).
static_assert(offsetof(input_report, buttons) == 2);
static_assert(offsetof(input_report, left_trigger) == 6);
static_assert(offsetof(input_report, right_trigger) == 8);
static_assert(offsetof(input_report, left_x) == 10);
static_assert(offsetof(input_report, left_y) == 12);
static_assert(offsetof(input_report, right_x) == 14);
static_assert(offsetof(input_report, right_y) == 16);
static_assert(offsetof(input_report, left_pad_x) == 18);
static_assert(offsetof(input_report, left_pad_y) == 20);
static_assert(offsetof(input_report, right_pad_x) == 24);
static_assert(offsetof(input_report, right_pad_y) == 26);
static_assert(offsetof(input_report, imu_timestamp) == 30);
static_assert(offsetof(input_report, accel_x) == 34);
static_assert(offsetof(input_report, accel_z) == 38);
static_assert(offsetof(input_report, gyro_x) == 40);
static_assert(offsetof(input_report, gyro_z) == 44);
static_assert(offsetof(input_report, quat_w) == 46);

// Report descriptor of the author's wired unit, captured on 2026-10-05 from the
// GET DESCRIPTOR (HID Report) response at plug-in (captures/sc26-report-descriptor-1.hex,
// captures/sc26-plugin.pcapng; firmware build time 0x6A4D85E3, bcdDevice 0x0307).
// Three top-level collections: the lizard-mode mouse (report 0x40) and keyboard
// (0x41) Windows binds to mouhid/kbdhid, and the vendor collection (usage page
// 0xFF00, usage 1) Steam opens, which carries the state, battery, haptic and
// feature reports. Windows enumerates them as three HID children
// (captures/steam-controller-hid.txt). 372 bytes.
inline constexpr std::uint8_t report_descriptor[] = {
  0x05, 0x01,                   // Usage Page (Generic Desktop)
  0x09, 0x02,                   // Usage (0x02)
  0xa1, 0x01,                   // Collection (Application)
  0x85, 0x40,                   //   Report ID (0x40)
  0x09, 0x01,                   //   Usage (0x01)
  0xa1, 0x00,                   //   Collection (Physical)
  0x05, 0x09,                   //     Usage Page (Button)
  0x19, 0x01,                   //     Usage Minimum (0x01)
  0x29, 0x02,                   //     Usage Maximum (0x02)
  0x15, 0x00,                   //     Logical Minimum (0)
  0x25, 0x01,                   //     Logical Maximum (1)
  0x75, 0x01,                   //     Report Size (1)
  0x95, 0x02,                   //     Report Count (2)
  0x81, 0x02,                   //     Input (Data,Var,Abs)
  0x75, 0x06,                   //     Report Size (6)
  0x95, 0x01,                   //     Report Count (1)
  0x81, 0x01,                   //     Input (Const,Array,Abs)
  0x05, 0x01,                   //     Usage Page (Generic Desktop)
  0x09, 0x30,                   //     Usage (0x30)
  0x09, 0x31,                   //     Usage (0x31)
  0x15, 0x81,                   //     Logical Minimum (-127)
  0x25, 0x7f,                   //     Logical Maximum (127)
  0x75, 0x08,                   //     Report Size (8)
  0x95, 0x02,                   //     Report Count (2)
  0x81, 0x06,                   //     Input (Data,Var,Rel)
  0x95, 0x01,                   //     Report Count (1)
  0x09, 0x38,                   //     Usage (0x38)
  0x81, 0x06,                   //     Input (Data,Var,Rel)
  0x05, 0x0c,                   //     Usage Page (Consumer)
  0x0a, 0x38, 0x02,             //     Usage (0x238)
  0x95, 0x01,                   //     Report Count (1)
  0x81, 0x06,                   //     Input (Data,Var,Rel)
  0xc0,                         //   End Collection (0)
  0xc0,                         // End Collection (0)
  0x05, 0x01,                   // Usage Page (Generic Desktop)
  0x09, 0x06,                   // Usage (0x06)
  0xa1, 0x01,                   // Collection (Application)
  0x85, 0x41,                   //   Report ID (0x41)
  0x05, 0x07,                   //   Usage Page (Keyboard)
  0x19, 0xe0,                   //   Usage Minimum (0xe0)
  0x29, 0xe7,                   //   Usage Maximum (0xe7)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x25, 0x01,                   //   Logical Maximum (1)
  0x75, 0x01,                   //   Report Size (1)
  0x95, 0x08,                   //   Report Count (8)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x81, 0x01,                   //   Input (Const,Array,Abs)
  0x19, 0x00,                   //   Usage Minimum (0x00)
  0x29, 0x65,                   //   Usage Maximum (0x65)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x25, 0x65,                   //   Logical Maximum (101)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x06,                   //   Report Count (6)
  0x81, 0x00,                   //   Input (Data,Array,Abs)
  0xc0,                         // End Collection (0)
  0x06, 0x00, 0xff,             // Usage Page (Vendor 0xFF00)
  0x09, 0x01,                   // Usage (0x01)
  0xa1, 0x01,                   // Collection (Application)
  0x85, 0x42,                   //   Report ID (0x42)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x35,                   //   Report Count (53)
  0x09, 0x42,                   //   Usage (0x42)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x85, 0x44,                   //   Report ID (0x44)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x05,                   //   Report Count (5)
  0x09, 0x44,                   //   Usage (0x44)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x85, 0x79,                   //   Report ID (0x79)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x01,                   //   Report Count (1)
  0x09, 0x79,                   //   Usage (0x79)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x85, 0x43,                   //   Report ID (0x43)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x0e,                   //   Report Count (14)
  0x09, 0x43,                   //   Usage (0x43)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x85, 0x7b,                   //   Report ID (0x7b)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x0c,                   //   Report Count (12)
  0x09, 0x7b,                   //   Usage (0x7b)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x85, 0x45,                   //   Report ID (0x45)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x2d,                   //   Report Count (45)
  0x09, 0x45,                   //   Usage (0x45)
  0x81, 0x02,                   //   Input (Data,Var,Abs)
  0x85, 0x80,                   //   Report ID (0x80)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x09,                   //   Report Count (9)
  0x09, 0x80,                   //   Usage (0x80)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x81,                   //   Report ID (0x81)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x07,                   //   Report Count (7)
  0x09, 0x81,                   //   Usage (0x81)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x82,                   //   Report ID (0x82)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x03,                   //   Report Count (3)
  0x09, 0x82,                   //   Usage (0x82)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x83,                   //   Report ID (0x83)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x09,                   //   Report Count (9)
  0x09, 0x83,                   //   Usage (0x83)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x84,                   //   Report ID (0x84)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x08,                   //   Report Count (8)
  0x09, 0x84,                   //   Usage (0x84)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x85,                   //   Report ID (0x85)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x03,                   //   Report Count (3)
  0x09, 0x85,                   //   Usage (0x85)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x86,                   //   Report ID (0x86)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x03,                   //   Report Count (3)
  0x09, 0x86,                   //   Usage (0x86)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x87,                   //   Report ID (0x87)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x3f,                   //   Report Count (63)
  0x09, 0x87,                   //   Usage (0x87)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x89,                   //   Report ID (0x89)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x3f,                   //   Report Count (63)
  0x09, 0x89,                   //   Usage (0x89)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x88,                   //   Report ID (0x88)
  0x15, 0x00,                   //   Logical Minimum (0)
  0x26, 0xff, 0x00,             //   Logical Maximum (255)
  0x75, 0x08,                   //   Report Size (8)
  0x95, 0x3f,                   //   Report Count (63)
  0x09, 0x88,                   //   Usage (0x88)
  0x91, 0x02,                   //   Output (Data,Var,Abs)
  0x85, 0x01,                   //   Report ID (0x01)
  0x95, 0x3f,                   //   Report Count (63)
  0x09, 0x01,                   //   Usage (0x01)
  0xb1, 0x02,                   //   Feature (Data,Var,Abs)
  0x85, 0x02,                   //   Report ID (0x02)
  0x95, 0x3f,                   //   Report Count (63)
  0x09, 0x01,                   //   Usage (0x01)
  0xb1, 0x02,                   //   Feature (Data,Var,Abs)
  0xc0,                         // End Collection (0)
};
inline constexpr std::size_t report_descriptor_size = sizeof(report_descriptor);
static_assert(report_descriptor_size == 372);
// The descriptor above is the real unit's, so find_profile() may hand out the profile.
inline constexpr bool report_descriptor_is_provisional = false;

// ---- Input encoding ------------------------------------------------------

// Normalized state in Vibeshine's conventions: buttons as a bitmask the caller
// has already translated to `button` bits; sticks +/-32767 positive up/right;
// triggers 0..255; pads 0..65535 with (0,0) top-left as the protocol's touch
// requests carry them; pressure 0..65535; motion in milli m/s^2 and milli dps
// in SDL axis order (x right, y up, z toward the player).
struct state {
  std::uint8_t sequence {};
  // Microsecond clock copied into every report, as the real unit does (its
  // timestamp advances ~3.8 ms per 250 Hz report). Steam integrates the gyro over
  // the deltas of this field, so the driver must stamp it from a real clock: a
  // counter that moved by 1 per IMU sample left Steam's gyro glyph motionless.
  std::uint32_t imu_timestamp {};
  // Capacitive grip sensors (left, right) report "held". A client with
  // LI_CCAP_GRIP_SENSE sends them as button bits; a client without it leaves
  // both released (the earlier motion-activity heuristic was removed).
  bool grip_touch[2] {};
  // Capacitive stick touch (left, right) from a client with LI_CCAP_STICK_TOUCH.
  // Used only once stick_touch_from_deflection is off; until then a deflected
  // stick counts as touched (the real unit reports touch with the thumb resting).
  bool stick_touch[2] {};
  bool pad_touched[2] {};
  std::uint16_t pad_x[2] {};
  std::uint16_t pad_y[2] {};
  std::uint16_t pad_pressure[2] {};
  std::int16_t accel[3] {0, 0, static_cast<std::int16_t>(accel_counts_per_g)};  // device x,y,z
  std::int16_t gyro[3] {};
  std::uint8_t battery_percent {100};
  std::uint8_t charge_state {charge_discharging};
  bool stick_touch_from_deflection {true};
  bool trigger_click_from_analog {true};

  void reset() noexcept { *this = state {}; }
};

[[nodiscard]] inline std::int16_t clamp_i16(const std::int64_t value) noexcept {
  return static_cast<std::int16_t>(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
}

[[nodiscard]] inline std::int16_t pad_axis_x(const std::uint16_t x) noexcept {
  // SDL: x_norm = raw / 65536 + 0.5  =>  raw = (x_norm - 0.5) * 65536
  return clamp_i16(static_cast<std::int32_t>(x) - 32768);
}

[[nodiscard]] inline std::int16_t pad_axis_y(const std::uint16_t y) noexcept {
  // SDL: y_norm = -raw / 65536 + 0.5  =>  raw = (0.5 - y_norm) * 65536
  return clamp_i16(32768 - static_cast<std::int32_t>(y));
}

[[nodiscard]] inline std::uint16_t pad_pressure(const std::uint16_t pressure) noexcept {
  return static_cast<std::uint16_t>(pressure >> 1);  // 0..65535 -> 0..32767
}

[[nodiscard]] inline std::int16_t trigger_axis(const std::uint8_t trigger) noexcept {
  return static_cast<std::int16_t>((static_cast<std::int32_t>(trigger) * 32767) / 255);
}

// Threshold at which the firmware reports the trigger's end-stop click.
inline constexpr std::uint8_t trigger_click_threshold = 0xF0;
// Deflection beyond which a stick counts as touched when the client cannot
// report capacitive touch itself.
inline constexpr std::int32_t stick_touch_deflection = 3276;

[[nodiscard]] inline input_report encode_input(
  const std::uint32_t buttons,
  const std::int16_t left_x, const std::int16_t left_y,
  const std::int16_t right_x, const std::int16_t right_y,
  const std::uint8_t left_trigger, const std::uint8_t right_trigger,
  state &st) noexcept {
  input_report r {};
  r.report_id = input_report_id;
  r.sequence = st.sequence++;
  std::uint32_t bits = buttons;
  if (st.trigger_click_from_analog) {
    if (left_trigger >= trigger_click_threshold) bits |= btn_left_trigger_click;
    if (right_trigger >= trigger_click_threshold) bits |= btn_right_trigger_click;
  }
  if (st.pad_touched[0]) bits |= btn_left_pad_touch;
  if (st.pad_touched[1]) bits |= btn_right_pad_touch;
  if (st.grip_touch[0]) bits |= btn_left_grip_touch;
  if (st.grip_touch[1]) bits |= btn_right_grip_touch;
  if (st.stick_touch[0]) bits |= btn_left_stick_touch;
  if (st.stick_touch[1]) bits |= btn_right_stick_touch;
  if (st.stick_touch_from_deflection) {
    const auto deflected = [](const std::int16_t x, const std::int16_t y) {
      const std::int32_t ax = x < 0 ? -static_cast<std::int32_t>(x) : x;
      const std::int32_t ay = y < 0 ? -static_cast<std::int32_t>(y) : y;
      return ax > stick_touch_deflection || ay > stick_touch_deflection;
    };
    if (deflected(left_x, left_y)) bits |= btn_left_stick_touch;
    if (deflected(right_x, right_y)) bits |= btn_right_stick_touch;
  }
  r.buttons = bits;
  r.left_trigger = trigger_axis(left_trigger);
  r.right_trigger = trigger_axis(right_trigger);
  r.left_x = left_x;
  r.left_y = left_y;
  r.right_x = right_x;
  r.right_y = right_y;
  if (st.pad_touched[0]) {
    r.left_pad_x = pad_axis_x(st.pad_x[0]);
    r.left_pad_y = pad_axis_y(st.pad_y[0]);
    r.left_pressure = pad_pressure(st.pad_pressure[0]);
  }
  if (st.pad_touched[1]) {
    r.right_pad_x = pad_axis_x(st.pad_x[1]);
    r.right_pad_y = pad_axis_y(st.pad_y[1]);
    r.right_pressure = pad_pressure(st.pad_pressure[1]);
  }
  r.imu_timestamp = st.imu_timestamp;
  r.accel_x = st.accel[0];
  r.accel_y = st.accel[1];
  r.accel_z = st.accel[2];
  r.gyro_x = st.gyro[0];
  r.gyro_y = st.gyro[1];
  r.gyro_z = st.gyro[2];
  // No orientation filter runs here; identity quaternion.
  r.quat_w = 32767;
  return r;
}

// The button word an input report is classified on for queueing (an edge makes the
// report a transition that survives until the host reads it). The stick-touch bits
// derived from stick deflection (clients without the stick-touch capability) are
// left out: a stick resting on the threshold would otherwise turn every snapshot
// into a transition and crowd real button edges out of the transition queue while
// the host has no read pending. A client's own stick-touch bits are real edges and
// stay in.
[[nodiscard]] inline constexpr std::uint32_t classify_buttons(const state &st,
                                                              std::uint32_t buttons) noexcept {
  if (st.stick_touch_from_deflection) {
    buttons &= ~(btn_left_stick_touch | btn_right_stick_touch);
  }
  return buttons;
}

// Motion in SDL axis order -> device axes. SDL reads device X as x, device Z as
// y, and -device Y as z, so the inverse is x->X, y->Z, -z->Y.
inline void apply_accel_milli(state &st, const std::int32_t x, const std::int32_t y,
                              const std::int32_t z) noexcept {
  // Takes a 64-bit value so negating INT32_MIN (a rogue client) is not signed overflow.
  const auto convert = [](const std::int64_t milli) {
    return clamp_i16((milli * accel_counts_per_g) / milli_g);
  };
  st.accel[0] = convert(x);
  st.accel[2] = convert(y);
  st.accel[1] = convert(-static_cast<std::int64_t>(z));
}

inline void apply_gyro_milli(state &st, const std::int32_t x, const std::int32_t y,
                             const std::int32_t z) noexcept {
  // Takes a 64-bit value so negating INT32_MIN (a rogue client) is not signed overflow.
  const auto convert = [](const std::int64_t milli) {
    return clamp_i16((milli * gyro_counts_per_1000_dps) / 1000000);
  };
  st.gyro[0] = convert(x);
  st.gyro[2] = convert(y);
  st.gyro[1] = convert(-static_cast<std::int64_t>(z));
}

[[nodiscard]] inline battery_report encode_battery(const state &st) noexcept {
  battery_report r {};
  r.report_id = battery_report_id;
  r.charge_state = st.charge_state;
  r.battery_level = st.battery_percent > 100 ? 100 : st.battery_percent;
  // Pack figures shaped like the author's unit on USB at 100 %: 4122 mV cell,
  // 4160 mV system, 4980 mV input, 157 mA, 239 mA input, temperature 0x76ED.
  // Nothing on the host side acts on them.
  const bool on_usb = st.charge_state == charge_charging || st.charge_state == charge_done;
  r.battery_voltage_mv = static_cast<std::uint16_t>(3522 + (r.battery_level * 6));  // 4122 at 100 %
  r.system_voltage_mv = 4160;
  r.input_voltage_mv = on_usb ? 4980 : 0;
  r.current_ma = on_usb ? 157 : 0;
  r.input_current_ma = on_usb ? 239 : 0;
  r.temperature = 0x76ED;
  return r;
}

// ---- Feature-report control channel ---------------------------------------

// Values the author's unit returned to GET_ATTRIBUTES_VALUES on 2026-10-05
// (captures/sc26-steam-handshake.txt): firmware built 2026-07-07 23:04 UTC,
// bootloader 2025-09-23, board revision 74. Steam saw these on a unit it did
// not ask to update. The reply carries exactly these six tags in this order;
// the unit sent no UNIQUE_ID. These are defaults: a host that has read the
// real controller's values replaces them per slot (apply_attributes), so Steam
// offers a firmware update only when the real controller needs one.
inline constexpr std::uint32_t captured_firmware_build_time = 0x6A4D85E3;
inline constexpr std::uint32_t captured_bootloader_build_time = 0x68D2F92E;
inline constexpr std::uint32_t captured_board_revision = 0x4A;
inline constexpr std::uint32_t captured_capabilities = 0;
inline constexpr std::uint32_t captured_connection_interval_us = 4000;
// 12-character build id the unit returned in GET_DEVICE_INFO 0 next to the
// firmware build time; a hash-shaped placeholder here, not the unit's.
inline constexpr char device_info_build_id[] = "000000000000";
[[nodiscard]] constexpr std::array<char, 16> default_build_id() noexcept {
  std::array<char, 16> id {};
  for (std::size_t i = 0; i < id.size() && device_info_build_id[i] != '\0'; ++i) id[i] = device_info_build_id[i];
  return id;
}

struct attributes {
  std::uint32_t unique_id {};  // Not reported by the firmware; kept for callers.
  // Qualified: the member name would otherwise shadow the namespace constant.
  std::uint32_t product_id {lvg::sc26_usb::product_id};
  std::uint32_t capabilities {captured_capabilities};
  std::uint32_t firmware_build_time {captured_firmware_build_time};
  std::uint32_t board_revision {captured_board_revision};
  std::uint32_t bootloader_build_time {captured_bootloader_build_time};
  std::uint32_t connection_interval_us {captured_connection_interval_us};
};

struct feature_state {
  attributes attrs {};
  // Real units: unit serial "FXA" + 10 digits, board serial "MXA" + 10 chars.
  // Synthetic here; never a real unit's.
  std::array<char, 16> unit_serial {"LVGSC260000"};
  std::array<char, 16> board_serial {"LVGSC26BOARD0"};
  // The 12-character build id GET_DEVICE_INFO 0 returns; the placeholder until
  // the host forwards the real controller's.
  std::array<char, 16> build_id {default_build_id()};
  // The settings table is the source of truth; lizard_mode/imu_mode mirror entries 9 and
  // 48 so a GET_SETTINGS_VALUES answer and the flags never disagree (a fresh unit and a
  // LOAD_DEFAULT_SETTINGS both report lizard mode on).
  std::array<std::uint16_t, setting_count> settings {default_settings()};
  bool lizard_mode {true};
  std::uint8_t imu_mode {};
  // Reply to the most recent command, read back with GetFeature.
  std::uint8_t reply[feature_report_size] {};
  bool have_reply {};
  std::uint8_t last_unknown_command {};
  std::uint32_t unknown_commands {};

  // Restores every default. The driver keeps this struct in zero-initialised
  // WDF context memory where no constructor ever ran, so nothing from the
  // previous contents may be preserved here: an earlier version kept the
  // attributes and serials and thereby kept zeros, and Steam read an all-zero
  // attribute list and an empty serial from the first test rig. Callers that
  // customise the serial do so after reset().
  void reset() noexcept { *this = feature_state {}; }
};

// ---- Attributes forwarded from a real controller --------------------------

// Which fields of an attribute_update carry a value read from the real
// controller. Same values as protocol.h steam_controller_attribute_flags (the
// driver pins that); the portable contract has its own so the responder stays
// Windows-free.
inline constexpr std::uint32_t attr_update_firmware_build_time = 0x01;
inline constexpr std::uint32_t attr_update_bootloader_build_time = 0x02;
inline constexpr std::uint32_t attr_update_board_revision = 0x04;
inline constexpr std::uint32_t attr_update_capabilities = 0x08;
inline constexpr std::uint32_t attr_update_build_id = 0x40;
inline constexpr std::uint32_t attr_update_serials = 0x80;

struct attribute_update {
  std::uint32_t flags {};
  std::uint32_t firmware_build_time {};
  std::uint32_t bootloader_build_time {};
  std::uint32_t board_revision {};
  std::uint32_t capabilities {};
  char build_id[16] {};
  char unit_serial[16] {};
  char board_serial[16] {};
};

// True when `text` holds at least one character before its first NUL (or the
// end of the 16 bytes) and every character up to there is printable ASCII
// (0x20..0x7E): what the control channel may echo to Steam as a serial or
// build id. Anything else is refused whole; nothing is rewritten.
[[nodiscard]] inline bool printable_attribute_string(const char (&text)[16]) noexcept {
  std::size_t n = 0;
  while (n < 16 && text[n] != '\0') {
    const unsigned char c = static_cast<unsigned char>(text[n]);
    if (c < 0x20 || c > 0x7E) return false;
    ++n;
  }
  return n != 0;
}

// Copies a validated string into a reply field: at most 15 characters, NUL
// padded, always terminated at [15] so a reader that scans finds an end.
inline void store_attribute_string(std::array<char, 16> &out, const char (&text)[16]) noexcept {
  out.fill('\0');
  for (std::size_t n = 0; n < 15 && text[n] != '\0'; ++n) out[n] = text[n];
}

// Replaces the flagged fields of `fs` with the real controller's. The product
// id (0x1302: the virtual device is wired, whatever the real one is on) and the
// connection interval are never touched, and a serial or build id that is not
// printable_attribute_string keeps its current value, so a host that read
// nothing useful leaves the synthetic per-slot serial in place. Returns the
// flags actually applied.
inline std::uint32_t apply_attributes(const attribute_update &update, feature_state &fs) noexcept {
  std::uint32_t applied = 0;
  if (update.flags & attr_update_firmware_build_time) {
    fs.attrs.firmware_build_time = update.firmware_build_time;
    applied |= attr_update_firmware_build_time;
  }
  if (update.flags & attr_update_bootloader_build_time) {
    fs.attrs.bootloader_build_time = update.bootloader_build_time;
    applied |= attr_update_bootloader_build_time;
  }
  if (update.flags & attr_update_board_revision) {
    fs.attrs.board_revision = update.board_revision;
    applied |= attr_update_board_revision;
  }
  if (update.flags & attr_update_capabilities) {
    fs.attrs.capabilities = update.capabilities;
    applied |= attr_update_capabilities;
  }
  if ((update.flags & attr_update_build_id) && printable_attribute_string(update.build_id)) {
    store_attribute_string(fs.build_id, update.build_id);
    applied |= attr_update_build_id;
  }
  if (update.flags & attr_update_serials) {
    // Each serial stands on its own: a unit serial with an empty board serial
    // still replaces the unit serial.
    if (printable_attribute_string(update.unit_serial)) {
      store_attribute_string(fs.unit_serial, update.unit_serial);
      applied |= attr_update_serials;
    }
    if (printable_attribute_string(update.board_serial)) {
      store_attribute_string(fs.board_serial, update.board_serial);
      applied |= attr_update_serials;
    }
  }
  return applied;
}

inline void put_le16(std::uint8_t *p, const std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put_le32(std::uint8_t *p, const std::uint32_t v) noexcept {
  put_le16(p, static_cast<std::uint16_t>(v));
  put_le16(p + 2, static_cast<std::uint16_t>(v >> 16));
}
[[nodiscard]] inline std::uint16_t get_le16(const std::uint8_t *p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// Handles a SetFeature on the control channel. `data` starts at the report id
// (data[0] == features_report_id); `size` is the HID transfer length. Returns
// false for a malformed transfer; unknown commands are acknowledged with an
// empty reply and counted so the host can surface them.
[[nodiscard]] inline bool set_feature(const std::uint8_t *data, const std::size_t size,
                                      feature_state &fs) noexcept {
  if (data == nullptr || size < 3 || data[0] != features_report_id) return false;
  const std::uint8_t type = data[1];
  const std::uint8_t length = data[2];
  const std::uint8_t *payload = data + 3;
  const std::size_t available = size - 3;
  if (length > available) return false;

  std::memset(fs.reply, 0, sizeof(fs.reply));
  fs.reply[0] = features_report_id;
  fs.reply[1] = type;
  fs.reply[2] = 0;
  fs.have_reply = true;
  std::uint8_t *out = fs.reply + 3;
  constexpr std::size_t out_capacity = feature_report_size - 3;

  switch (type) {
    case cmd_clear_digital_mappings:
    case cmd_set_default_digital_mappings:
      return true;
    case cmd_load_default_settings:
      fs.settings = default_settings();
      fs.lizard_mode = true;
      fs.imu_mode = 0;
      return true;
    case cmd_set_settings_values: {
      for (std::size_t i = 0; i + 3 <= length; i += 3) {
        const std::uint8_t id = payload[i];
        const std::uint16_t value = get_le16(payload + i + 1);
        if (id < setting_count) fs.settings[id] = value;

        if (id == setting_lizard_mode) fs.lizard_mode = value != 0;
        if (id == setting_imu_mode) fs.imu_mode = static_cast<std::uint8_t>(value);
      }
      return true;
    }
    case cmd_get_settings_values:
    case cmd_get_settings_defaults:
    case cmd_get_settings_maxs: {
      // Request payload lists the setting ids wanted; an empty request returns
      // the first ids that fit. Replies are (id, value) triples.
      std::size_t n = 0;
      const std::size_t max_triples = out_capacity / 3;
      if (length == 0) {
        for (std::uint8_t id = 0; id < setting_count && n < max_triples; ++id, ++n) {
          out[n * 3] = id;
          put_le16(out + n * 3 + 1, type == cmd_get_settings_values ? fs.settings[id]
                                     : type == cmd_get_settings_maxs ? 0xFFFFu : default_setting(id));
        }
      } else {
        for (std::size_t i = 0; i < length && n < max_triples; ++i, ++n) {
          const std::uint8_t id = payload[i];
          out[n * 3] = id;
          const std::uint16_t v = id < setting_count && type == cmd_get_settings_values ? fs.settings[id]
                                  : type == cmd_get_settings_maxs ? 0xFFFFu
                                  : id < setting_count ? default_setting(id) : 0u;
          put_le16(out + n * 3 + 1, v);
        }
      }
      fs.reply[2] = static_cast<std::uint8_t>(n * 3);
      return true;
    }
    case cmd_get_attributes_values: {
      // Same tags, same order, as the real unit's 30-byte reply.
      const struct { std::uint8_t tag; std::uint32_t value; } list[] = {
        {attr_product_id, fs.attrs.product_id},
        {attr_capabilities, fs.attrs.capabilities},
        {attr_bootloader_build_time, fs.attrs.bootloader_build_time},
        {attr_firmware_build_time, fs.attrs.firmware_build_time},
        {attr_board_revision, fs.attrs.board_revision},
        {attr_connection_interval_us, fs.attrs.connection_interval_us},
      };
      std::size_t n = 0;
      for (const auto &a : list) {
        if ((n + 1) * 5 > out_capacity) break;
        out[n * 5] = a.tag;
        put_le32(out + n * 5 + 1, a.value);
        ++n;
      }
      fs.reply[2] = static_cast<std::uint8_t>(n * 5);
      return true;
    }
    case cmd_get_string_attribute: {
      // Request: [0x15][tag]; reply: always 20 bytes, [tag][string, NUL padded],
      // as the real unit answers (hid-steam reads the serial right after the tag).
      const std::uint8_t tag = length >= 1 ? payload[0] : string_attr_unit_serial;
      out[0] = tag;
      const char *text = tag == string_attr_board_serial ? fs.board_serial.data() : fs.unit_serial.data();
      const std::size_t text_size = tag == string_attr_board_serial ? fs.board_serial.size() : fs.unit_serial.size();
      std::size_t n = 0;
      // Bound first, then read: a serial that fills its array has no terminator to find.
      while (n < text_size && n + 1 < string_attr_reply_length && text[n] != '\0') {
        out[1 + n] = static_cast<std::uint8_t>(text[n]);
        ++n;
      }
      fs.reply[2] = string_attr_reply_length;
      return true;
    }
    case cmd_write_c1:
    case cmd_write_dc:
    case cmd_write_e2:
      // Steam writes these once per connect and reads nothing back.
      return true;
    case cmd_get_keyed_value: {
      // Payload is an ASCII key (NUL included). In the capture
      // (sc26-steam-control.tsv, decoded in sc26-steam-handshake.txt) the unit
      // answered "user/wireless_transport" with one byte (0: USB), "esb/bond"
      // with a 24-byte bond record naming its puck, and "esb/bond_2" with
      // nothing. A wired-only virtual unit has no puck, so both bond keys get
      // the empty reply and only the transport key answers.
      constexpr char k_transport[] = "user/wireless_transport";
      if (length == sizeof(k_transport) && std::memcmp(payload, k_transport, sizeof(k_transport)) == 0) {
        out[0] = 0;
        fs.reply[2] = 1;
      }
      return true;
    }
    case cmd_get_device_info: {
      // Sub id 0: [00][firmware build time u32][board revision u32][build id,
      // 16 bytes NUL padded][unit serial, 16 bytes NUL padded] (41 bytes).
      // Sub id 1: [01] + 33 zero bytes. Sub id 2: [02][u16 counter][01][5 zero bytes].
      const std::uint8_t sub = length >= 1 ? payload[0] : 0;
      out[0] = sub;
      if (sub == 0) {
        put_le32(out + 1, fs.attrs.firmware_build_time);
        put_le32(out + 5, fs.attrs.board_revision);
        for (std::size_t i = 0; i < 16 && i < fs.build_id.size() && fs.build_id[i] != '\0'; ++i) {
          out[9 + i] = static_cast<std::uint8_t>(fs.build_id[i]);
        }
        for (std::size_t i = 0; i < 16 && i < fs.unit_serial.size() && fs.unit_serial[i] != '\0'; ++i) {
          out[25 + i] = static_cast<std::uint8_t>(fs.unit_serial[i]);
        }
        fs.reply[2] = 41;
      } else if (sub == 1) {
        fs.reply[2] = 34;
      } else {
        out[3] = 1;
        fs.reply[2] = 9;
      }
      return true;
    }
    case cmd_trigger_haptic_pulse:
      // Legacy pulse command on the control channel; the output report is the
      // 2026 path. Accept it so a host that still uses it is not refused.
      return true;
    default:
      fs.last_unknown_command = type;
      ++fs.unknown_commands;
      return true;
  }
}

// Answers a GetFeature on the control channel with the reply to the last
// command. Returns the number of bytes written (always the full report), or 0
// when the buffer cannot hold it.
[[nodiscard]] inline std::size_t get_feature(std::uint8_t *buffer, const std::size_t capacity,
                                             const feature_state &fs) noexcept {
  if (buffer == nullptr || capacity < feature_report_size) return 0;
  std::memset(buffer, 0, capacity);
  if (fs.have_reply) {
    std::memcpy(buffer, fs.reply, feature_report_size);
  } else {
    buffer[0] = features_report_id;
  }
  return feature_report_size;
}

// ---- Haptic output ---------------------------------------------------------

struct rumble {
  std::uint16_t left {};   // 0..65535
  std::uint16_t right {};
};

// Decodes the two haptic output reports a host uses for rumble. Pulses become a
// magnitude from their duty cycle so a client without pad actuators can still
// render them. Returns false for other report ids or short transfers.
[[nodiscard]] inline bool decode_haptic_output(const std::uint8_t *data, const std::size_t size,
                                               rumble &current) noexcept {
  if (data == nullptr || size < 2) return false;
  if (data[0] == haptic_rumble_id) {
    if (size < sizeof(haptic_rumble_report)) return false;
    current.left = get_le16(data + 4);
    current.right = get_le16(data + 7);
    return true;
  }
  if (data[0] == haptic_pulse_id) {
    if (size < haptic_pulse_report_size) return false;
    const std::uint16_t on_us = get_le16(data + 2);
    const std::uint16_t off_us = get_le16(data + 4);
    const std::uint16_t repeat = get_le16(data + 6);
    std::uint16_t magnitude = 0;
    if (repeat != 0 && on_us != 0) {
      const std::uint32_t period = static_cast<std::uint32_t>(on_us) + off_us;
      magnitude = static_cast<std::uint16_t>((static_cast<std::uint32_t>(on_us) * 65535u) / (period == 0 ? 1u : period));
    }
    if (data[1] == 1) current.left = magnitude; else current.right = magnitude;
    return true;
  }
  return false;
}

// The unit answers a zero-repeat pulse (Steam's per-side "stop") with a 6-byte
// 0x44 report: 44 04 02 00 00 00 for side 0, 44 03 02 00 00 00 for side 1
// (captures/sc26-steam-interrupt.tsv; other haptic reports get no answer).
// Writes that report to `ack` and returns its size, or 0 when `data` is not
// such a pulse.
[[nodiscard]] inline std::size_t encode_haptic_ack(const std::uint8_t *data, const std::size_t size,
                                                   std::uint8_t *ack, const std::size_t capacity) noexcept {
  if (data == nullptr || ack == nullptr || capacity < 6 || size < haptic_pulse_report_size ||
      data[0] != haptic_pulse_id) {
    return 0;
  }
  if (get_le16(data + 2) != 0 || get_le16(data + 4) != 0 || get_le16(data + 6) != 0) {
    return 0;
  }
  ack[0] = haptic_ack_report_id;
  ack[1] = data[1] == 0 ? 0x04 : 0x03;
  ack[2] = 0x02;
  ack[3] = ack[4] = ack[5] = 0;
  return 6;
}

}  // namespace lvg::sc26_usb
