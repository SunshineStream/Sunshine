// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include "libvirtualgamepad/protocol.h"
#include "libvirtualgamepad/keyboard_mouse_control.h"

namespace lvg {

// The real Steam Controller's firmware attributes, as the host read them from
// the controller the client is streaming from (sc26_usb.h names the tags).
// `flags` is steam_controller_attribute_flags: only flagged fields are
// applied, the rest keep the virtual device's defaults. Strings are ASCII,
// at most 15 characters, NUL padded; the driver keeps one only when it is
// non-empty printable ASCII, otherwise the synthetic per-slot serial or the
// placeholder build id stays.
struct steam_controller_attributes {
  std::uint32_t flags {};
  std::uint32_t firmware_build_time {};
  std::uint32_t bootloader_build_time {};
  std::uint32_t board_revision {};
  std::uint32_t capabilities {};
  char build_id[16] {};
  char unit_serial[16] {};
  char board_serial[16] {};
};

// Opens the driver's private WDF device interface, never a HID child path.
// A connection owns the controller slots it creates. Closing it releases those
// slots in the driver even if the caller did not explicitly destroy them.
// Thread safety: a connection may be used from one thread at a time; calls on
// the same connection from several threads must be serialised by the caller
// (the driver serialises per handle, the library does not). Distinct
// connections are independent.
//
// A controller is deleted when the machine sleeps, hibernates, or shuts down.
// Its next submit or poll returns ERROR_DEVICE_REMOVED; destroy_controller()
// then create_controller() on the same slot can restore it after resume.
// Creation may return ERROR_BUSY until PnP removes the previous child; callers
// should retry with their own bounded backoff, not block a suspend callback.
class client final {
 public:
  client() noexcept = default;
  ~client() noexcept;

  client(const client &) = delete;
  client &operator=(const client &) = delete;

  client(client &&other) noexcept;
  client &operator=(client &&other) noexcept;

  // Finds the first present Vibeshine VHF source-device interface and opens it
  // with the access required by the protocol IOCTLs. Returns ERROR_SUCCESS or
  // a Win32 error code.
  [[nodiscard]] DWORD connect() noexcept;
  void close() noexcept;

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] profile_mask_t available_profiles() const noexcept;
  [[nodiscard]] std::uint32_t available_features() const noexcept;
  [[nodiscard]] std::uint32_t maximum_controllers() const noexcept;

  // Error contract shared by every call below (Win32 codes):
  //   ERROR_INVALID_HANDLE     not connected
  //   ERROR_INVALID_PARAMETER  controller_id >= maximum_controllers(), a null
  //                            output pointer, or a submit whose header was
  //                            not initialised for this protocol version
  //   ERROR_NOT_SUPPORTED      create: the requested profile is not in
  //                            available_profiles(); submit_touch/motion/
  //                            battery: the slot's profile has no such fold
  //                            (Xbox profiles)
  //   ERROR_INVALID_PARAMETER  (also) a submit the profile cannot represent:
  //                            a touch contact index the pad does not have, a
  //                            move/hover on a contact that is not down, an
  //                            unknown motion kind
  //   ERROR_REVISION_MISMATCH  the driver rejected a request we built
  //                            (STATUS_INVALID_BUFFER_SIZE): a different
  //                            protocol generation
  //   ERROR_BUSY               create: the slot is occupied (by this or another
  //                            handle, or still stopping)
  //   ERROR_NOT_FOUND          the slot has no controller (destroy, submit,
  //                            poll_feedback)
  //   ERROR_ACCESS_DENIED      the slot belongs to another handle
  //   ERROR_NOT_READY          the device is stopping or stopped
  //   ERROR_INVALID_DATA       poll_feedback: the driver's event failed
  //                            validation (size, protocol version, slot, an
  //                            unknown feedback type, or a payload_size that
  //                            is not feedback_payload_size(type))
  //   ERROR_NO_MORE_ITEMS      poll_feedback: nothing fresh
  [[nodiscard]] DWORD create_controller(
    std::uint32_t controller_id,
    profile requested_profile) noexcept;
  [[nodiscard]] DWORD destroy_controller(std::uint32_t controller_id) noexcept;
  [[nodiscard]] DWORD submit_input_state(const input_state_request &input) noexcept;
  [[nodiscard]] DWORD submit_touch_state(const touch_state_request &input) noexcept;
  [[nodiscard]] DWORD submit_motion_state(const motion_state_request &input) noexcept;
  [[nodiscard]] DWORD submit_battery_state(const battery_state_request &input) noexcept;

  // Returns ERROR_NO_MORE_ITEMS when the driver's bounded feedback slot has
  // no fresh output. It does not block the streaming path.
  [[nodiscard]] DWORD poll_feedback(
    std::uint32_t controller_id,
    feedback_event *event) noexcept;

  // What the driver has discarded for this slot since it was created (see
  // feedback_counters): a diagnostic read, cheap enough to log on a timer.
  // ERROR_REVISION_MISMATCH here means the driver predates the counters; the
  // rest of the error contract above applies (ERROR_NOT_FOUND for an empty
  // slot, ERROR_ACCESS_DENIED for another handle's).
  [[nodiscard]] DWORD query_feedback_counters(
    std::uint32_t controller_id,
    feedback_counters *counters) noexcept;

  // Gives a Steam Controller slot the real controller's firmware attributes,
  // so Steam's GET_ATTRIBUTES_VALUES / GET_DEVICE_INFO / GET_STRING_ATTRIBUTE
  // see the real firmware build and offer an update only when the real
  // controller needs one. Send after create_controller, once per slot
  // creation (create resets them); a later call replaces them and Steam picks
  // the change up on its next poll. ERROR_REVISION_MISMATCH means the driver
  // predates the request (it keeps its captured defaults); ERROR_NOT_SUPPORTED
  // means the slot does not run profile::steam_controller; the rest of the
  // error contract above applies (ERROR_NOT_FOUND for an empty slot,
  // ERROR_ACCESS_DENIED for another handle's).
  [[nodiscard]] DWORD set_steam_controller_attributes(
    std::uint32_t controller_id,
    const steam_controller_attributes &attributes) noexcept;

  // Optional source-stage extension on this same owned control handle. No
  // protocol2/released capability or native identity qualification is implied.
  // The client (including connect/close/move) requires external serialization.
  [[nodiscard]] DWORD query_keyboard_mouse(km::query_response *response) noexcept;
  [[nodiscard]] DWORD open_keyboard_mouse() noexcept;
  // ERROR_BUSY means neutral drain is incomplete; retry boundedly. Success
  // means the child was synchronously removed after both neutrals retired.
  [[nodiscard]] DWORD close_keyboard_mouse() noexcept;
  // Force-remove only this owned KM child without closing controller slots on
  // the shared handle. Use after uncertain input/close timeout; no neutral proof.
  [[nodiscard]] DWORD abort_keyboard_mouse() noexcept;
  // ERROR_BUSY rejects before mutation. Any other transport failure after
  // acceptance is uncertain; never replay through SendInput before teardown.
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::keyboard_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::consumer_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::button_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::relative_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::absolute_request &input) noexcept;

 private:
  [[nodiscard]] DWORD issue_keyboard_mouse_submission(
    DWORD control_code, void *input, DWORD input_size) noexcept;
  [[nodiscard]] DWORD query_info() noexcept;
  [[nodiscard]] DWORD issue(
    DWORD control_code,
    void *input,
    DWORD input_size,
    void *output,
    DWORD output_size,
    DWORD *bytes_returned = nullptr) noexcept;

  HANDLE handle_ {INVALID_HANDLE_VALUE};
  query_info_response info_ {};
};

}  // namespace lvg
