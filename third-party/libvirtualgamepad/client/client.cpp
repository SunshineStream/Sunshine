// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "libvirtualgamepad/client.h"

#include <setupapi.h>

#include <cstring>
#include <utility>

namespace lvg {
namespace {

void initialize_header(request_header *const header, const std::uint32_t size) noexcept {
  header->size = size;
  header->version = k_protocol_version;
  header->reserved = 0;
}

}  // namespace

client::~client() noexcept {
  close();
}

client::client(client &&other) noexcept :
    handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
    info_(std::exchange(other.info_, query_info_response {})) {}

client &client::operator=(client &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    info_ = std::exchange(other.info_, query_info_response {});
  }
  return *this;
}

DWORD client::connect() noexcept {
  close();

  const HDEVINFO devices = SetupDiGetClassDevsW(
    &k_device_interface_guid,
    nullptr,
    nullptr,
    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devices == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }

  DWORD last_error = ERROR_NOT_FOUND;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(
          devices,
          nullptr,
          &k_device_interface_guid,
          index,
          &interface_data)) {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_ITEMS) {
        last_error = error;
      }
      break;
    }

    DWORD detail_bytes = 0;
    SetupDiGetDeviceInterfaceDetailW(
      devices,
      &interface_data,
      nullptr,
      0,
      &detail_bytes,
      nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        detail_bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
      last_error = GetLastError();
      continue;
    }

    auto *const detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(LocalAlloc(LPTR, detail_bytes));
    if (detail == nullptr) {
      last_error = ERROR_NOT_ENOUGH_MEMORY;
      continue;
    }
    detail->cbSize = sizeof(*detail);
    const BOOL detail_ok = SetupDiGetDeviceInterfaceDetailW(
      devices,
      &interface_data,
      detail,
      detail_bytes,
      nullptr,
      nullptr);
    if (!detail_ok) {
      last_error = GetLastError();
      LocalFree(detail);
      continue;
    }

    handle_ = CreateFileW(
      detail->DevicePath,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION | SECURITY_CONTEXT_TRACKING,
      nullptr);
    LocalFree(detail);
    if (handle_ == INVALID_HANDLE_VALUE) {
      last_error = GetLastError();
      continue;
    }

    const DWORD handshake = query_info();
    if (handshake == ERROR_SUCCESS) {
      last_error = ERROR_SUCCESS;
      break;
    }
    last_error = handshake;
    close();
  }

  SetupDiDestroyDeviceInfoList(devices);
  return last_error;
}

void client::close() noexcept {
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
  info_ = {};
}

bool client::connected() const noexcept {
  return handle_ != INVALID_HANDLE_VALUE;
}

profile_mask_t client::available_profiles() const noexcept {
  return info_.available_profiles;
}

std::uint32_t client::available_features() const noexcept {
  return info_.available_features;
}

std::uint32_t client::maximum_controllers() const noexcept {
  return info_.maximum_controllers;
}

DWORD client::create_controller(
  const std::uint32_t controller_id,
  const profile requested_profile) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }
  if (controller_id >= maximum_controllers()) {
    return ERROR_INVALID_PARAMETER;
  }
  if ((available_profiles() & profile_bit(requested_profile)) == 0) {
    return ERROR_NOT_SUPPORTED;
  }
  create_controller_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;
  request.requested_profile = requested_profile;
  return issue(ioctl_create_controller, &request, sizeof(request), nullptr, 0);
}

DWORD client::destroy_controller(const std::uint32_t controller_id) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }
  if (controller_id >= maximum_controllers()) {
    return ERROR_INVALID_PARAMETER;
  }
  controller_id_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;
  return issue(ioctl_destroy_controller, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_input_state(const input_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_input_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_touch_state(const touch_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_touch_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_motion_state(const motion_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved0[0] != 0 ||
      input.reserved0[1] != 0 || input.reserved0[2] != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_motion_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::submit_battery_state(const battery_state_request &input) noexcept {
  if (input.header.size != sizeof(input) || input.header.version != k_protocol_version ||
      input.header.reserved != 0 || input.reserved != 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue(ioctl_submit_battery_state, &request, sizeof(request), nullptr, 0);
}

DWORD client::poll_feedback(
  const std::uint32_t controller_id,
  feedback_event *const event) noexcept {
  if (event == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }

  controller_id_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;

  DWORD bytes = 0;
  const DWORD status = issue(
    ioctl_poll_feedback,
    &request,
    sizeof(request),
    event,
    sizeof(*event),
    &bytes);
  if (status != ERROR_SUCCESS) {
    return status;
  }
  if (bytes != sizeof(*event) || !valid_request(event, bytes) ||
      event->controller_id != controller_id ||
      event->payload_size > sizeof(event->payload) ||
      event->payload_size != feedback_payload_size(event->type)) {
    // An unknown type, or a payload that is not the whole struct of its type.
    return ERROR_INVALID_DATA;
  }
  return ERROR_SUCCESS;
}

DWORD client::query_keyboard_mouse(km::query_response *const response) noexcept {
  if (response == nullptr) return ERROR_INVALID_PARAMETER;
  *response = {};
  auto request = km::request<km::control_request>();
  km::query_response received {};
  DWORD bytes = 0;
  const DWORD status = issue(km::ioctl_query, &request, sizeof(request),
    &received, sizeof(received), &bytes);
  if (status != ERROR_SUCCESS) return status;
  if (bytes != sizeof(received) || !km::valid(received) || received.reserved != 0 ||
      (received.capabilities & ~(km::k_required_capabilities | km::primary_absolute_mouse | km::consumer)) != 0 ||
      (received.capabilities & km::k_relative_capabilities) != km::k_relative_capabilities ||
      received.queue_capacity < 2 ||
      ((received.capabilities & km::primary_absolute_mouse) != 0 &&
       (received.capabilities & km::bound_absolute_mouse) == 0) ||
      (((received.capabilities & km::bound_absolute_mouse) != 0) !=
       (received.absolute_target_generation != 0))) {
    return ERROR_INVALID_DATA;
  }
  *response = received;
  return ERROR_SUCCESS;
}

DWORD client::open_keyboard_mouse() noexcept {
  auto request = km::request<km::control_request>();
  return issue(km::ioctl_open, &request, sizeof(request), nullptr, 0);
}
DWORD client::close_keyboard_mouse() noexcept {
  auto request = km::request<km::control_request>();
  return issue(km::ioctl_close, &request, sizeof(request), nullptr, 0);
}
DWORD client::abort_keyboard_mouse() noexcept {
  auto request = km::request<km::control_request>();
  return issue(km::ioctl_abort, &request, sizeof(request), nullptr, 0);
}
DWORD client::submit_keyboard_mouse(const km::keyboard_request &input) noexcept {
  if (!km::valid(input) || input.reserved != 0 || input.pressed > 1 ||
      input.usage < 4 || input.usage > 0xe7) return ERROR_INVALID_PARAMETER;
  auto request = input;
  return issue_keyboard_mouse_submission(km::ioctl_keyboard, &request, sizeof(request));
}
DWORD client::submit_keyboard_mouse(const km::button_request &input) noexcept {
  if (!km::valid(input) || input.reserved != 0 || input.pressed > 1 ||
      input.button < 1 || input.button > 5) return ERROR_INVALID_PARAMETER;
  auto request = input;
  return issue_keyboard_mouse_submission(km::ioctl_button, &request, sizeof(request));
}
DWORD client::submit_keyboard_mouse(const km::consumer_request &input) noexcept {
  if (!km::valid(input) || input.reserved != 0 || input.pressed > 1) return ERROR_INVALID_PARAMETER;
  switch (input.usage) {
    case 0x00e2: case 0x00e9: case 0x00ea: case 0x00cd:
    case 0x00b5: case 0x00b6: case 0x00b7: break;
    default: return ERROR_INVALID_PARAMETER;
  }
  auto request = input;
  return issue_keyboard_mouse_submission(km::ioctl_consumer, &request, sizeof(request));
}
DWORD client::submit_keyboard_mouse(const km::relative_request &input) noexcept {
  if (!km::valid(input) || input.reserved != 0 || input.wheel == -128 ||
      input.pan == -128) return ERROR_INVALID_PARAMETER;
  auto request = input;
  return issue_keyboard_mouse_submission(km::ioctl_relative, &request, sizeof(request));
}
DWORD client::submit_keyboard_mouse(const km::absolute_request &input) noexcept {
  if (!km::valid(input)) return ERROR_INVALID_PARAMETER;
  auto request = input;
  return issue_keyboard_mouse_submission(km::ioctl_absolute, &request, sizeof(request));
}

DWORD client::issue_keyboard_mouse_submission(const DWORD code, void *const input,
  const DWORD input_size) noexcept {
  km::submission_response response {};
  DWORD bytes = 0;
  const DWORD status = issue(code, input, input_size, &response, sizeof(response), &bytes);
  // Only a validated producer reply can establish pre-admission backpressure.
  // A transport-origin busy may follow unknown delivery and is not retry-safe.
  if (status != ERROR_SUCCESS) return status == ERROR_BUSY ? ERROR_NOT_READY : status;
  if (bytes != sizeof(response) || !km::valid(response) || response.reserved != 0) return ERROR_INVALID_DATA;
  switch (response.acceptance) {
    case km::submission_acceptance::accepted: return ERROR_SUCCESS;
    case km::submission_acceptance::busy: return ERROR_BUSY;
  }
  return ERROR_INVALID_DATA;
}

DWORD client::query_info() noexcept {
  query_info_request request {};
  initialize_header(&request.header, sizeof(request));

  query_info_response response {};
  DWORD bytes = 0;
  const DWORD status = issue(
    ioctl_query_info,
    &request,
    sizeof(request),
    &response,
    sizeof(response),
    &bytes);
  if (status != ERROR_SUCCESS) {
    return status;
  }
  if (bytes != sizeof(response) || !valid_request(&response, bytes) ||
      response.minimum_protocol_version > k_protocol_version ||
      response.maximum_protocol_version < k_protocol_version ||
      response.maximum_controllers == 0 || response.maximum_controllers > k_max_controllers ||
      response.counters.haptic_evicted != 0 || response.counters.settings_evicted != 0 ||
      response.counters.transitions_dropped != 0 || response.counters.unknown_commands != 0) {
    // The device query carries no counters: a non-zero word there is the
    // other protocol generation, as it was before the word had a meaning.
    return ERROR_REVISION_MISMATCH;
  }
  info_ = response;
  return ERROR_SUCCESS;
}

DWORD client::query_feedback_counters(
  const std::uint32_t controller_id,
  feedback_counters *const counters) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }
  if (counters == nullptr || controller_id >= maximum_controllers()) {
    return ERROR_INVALID_PARAMETER;
  }

  controller_id_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;

  query_info_response response {};
  DWORD bytes = 0;
  const DWORD status = issue(
    ioctl_query_info,
    &request,
    sizeof(request),
    &response,
    sizeof(response),
    &bytes);
  if (status != ERROR_SUCCESS) {
    return status;
  }
  if (bytes != sizeof(response) || !valid_request(&response, bytes)) {
    return ERROR_INVALID_DATA;
  }
  *counters = response.counters;
  return ERROR_SUCCESS;
}

DWORD client::set_steam_controller_attributes(
  const std::uint32_t controller_id,
  const steam_controller_attributes &attributes) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }
  if (controller_id >= maximum_controllers()) {
    return ERROR_INVALID_PARAMETER;
  }

  steam_controller_attributes_request request {};
  initialize_header(&request.header, sizeof(request));
  request.controller_id = controller_id;
  request.flags = attributes.flags;
  request.firmware_build_time = attributes.firmware_build_time;
  request.bootloader_build_time = attributes.bootloader_build_time;
  request.board_revision = attributes.board_revision;
  request.capabilities = attributes.capabilities;
  static_assert(sizeof(request.build_id) == sizeof(attributes.build_id));
  static_assert(sizeof(request.unit_serial) == sizeof(attributes.unit_serial));
  static_assert(sizeof(request.board_serial) == sizeof(attributes.board_serial));
  std::memcpy(request.build_id, attributes.build_id, sizeof(request.build_id));
  std::memcpy(request.unit_serial, attributes.unit_serial, sizeof(request.unit_serial));
  std::memcpy(request.board_serial, attributes.board_serial, sizeof(request.board_serial));

  const DWORD status = issue(
    ioctl_set_steam_controller_attributes,
    &request,
    sizeof(request),
    nullptr,
    0);
  // A driver built before this request does not know its control code: WDF
  // fails it with STATUS_INVALID_DEVICE_REQUEST (ERROR_INVALID_FUNCTION). That
  // is the other protocol generation, like a rejected size, not a fault of the
  // call; the driver keeps its captured defaults and the caller carries on.
  return status == ERROR_INVALID_FUNCTION ? ERROR_REVISION_MISMATCH : status;
}

DWORD client::issue(
  const DWORD control_code,
  void *const input,
  const DWORD input_size,
  void *const output,
  const DWORD output_size,
  DWORD *const bytes_returned) noexcept {
  if (!connected()) {
    return ERROR_INVALID_HANDLE;
  }

  DWORD bytes = 0;
  if (!DeviceIoControl(
        handle_,
        control_code,
        input,
        input_size,
        output,
        output_size,
        &bytes,
        nullptr)) {
    const DWORD error = GetLastError();
    // The driver pins every request to its own protocol version and size. We
    // built every buffer ourselves, so the only way one is "invalid" is a driver
    // from a different protocol generation: report that rather than a buffer
    // fault, on every IOCTL and not only the handshake.
    return error == ERROR_INVALID_USER_BUFFER ? ERROR_REVISION_MISMATCH : error;
  }
  if (bytes_returned != nullptr) {
    *bytes_returned = bytes;
  }
  return ERROR_SUCCESS;
}

}  // namespace lvg
