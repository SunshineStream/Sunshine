// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <winioctl.h>
#include "keyboard_mouse.h"

namespace lvg::km {
// Keyboard/mouse extension, independently queried from gamepad protocol 2.
struct control_request { header prefix; };
static_assert(sizeof(control_request) == 8);
#pragma pack(push, 1)
enum class submission_acceptance : std::uint32_t { accepted = 0, busy = 1 };
struct submission_response {
  header prefix;
  submission_acceptance acceptance;
  std::uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(submission_response) == 16);
inline constexpr std::uint32_t k_relative_capabilities =
  keyboard | relative_mouse | wheel_detents;
inline constexpr std::uint32_t k_primary_capabilities =
  k_relative_capabilities | bound_absolute_mouse | primary_absolute_mouse | consumer;
// Identifies the primary-display coordinate domain. Hosts must recalculate
// their coordinate transform when display geometry changes.
inline constexpr std::uint32_t k_primary_absolute_generation = 1;
inline constexpr DWORD ioctl_query =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_open =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_close =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_keyboard =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x823, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_button =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x824, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_relative =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x825, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_absolute =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x826, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_abort =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x827, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_consumer =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x828, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
// Submission-only result contract. Busy is exclusively pre-admission and
// retry-safe only after a validated submission_response. Raw transport busy
// is normalized to uncertain by the client. All other errors are conservatively uncertain; even a transport
// error may follow server acceptance. Quiesce/abort before choosing a backend.
enum class submission_outcome { accepted, retry_without_acceptance, uncertain };
[[nodiscard]] constexpr submission_outcome classify_submission(const DWORD error) noexcept {
  return error == ERROR_SUCCESS ? submission_outcome::accepted :
    (error == ERROR_BUSY ? submission_outcome::retry_without_acceptance : submission_outcome::uncertain);
}
// abort: ERROR_SUCCESS means synchronous child deletion completed (or already
// absent), without proving neutral delivery. Any error leaves deletion unproven.
} // namespace lvg::km
