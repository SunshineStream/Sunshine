// Device-identity predicates shared by the probes and their tests.
#pragma once

#include <optional>
#include <string_view>

namespace lvg {

// True for a HID interface path carrying the Steam Controller (2026) identity,
// whether ours or a physical unit's. The path alone cannot tell them apart: a
// physical unit's collections can enumerate as "vid_28de&pid_1302&col03" with
// no "&mi_" segment, so this is only the cheap pre-filter in front of
// virtual_sc26_child_slot(). `lower_path` is the device interface path
// lower-cased by the caller.
[[nodiscard]] constexpr bool is_sc26_hid_path(const std::wstring_view lower_path) noexcept {
  return lower_path.find(L"vid_28de&pid_1302") != std::wstring_view::npos;
}

// The controller slot whose VHF child `lower_parent_instance_id` names, or
// nothing. The driver stamps each child it creates with the instance ID
// "VibeshineGamepad<slot>" (format_instance_id), which Windows keeps as the last
// '&'-separated segment of the child's device instance ID; the HID collections
// a probe enumerates are that child's children, so the collection's parent
// carries it. A physical unit's collections hang off a USB interface device
// ("usb\vid_28de&pid_1302&mi_02\...") and never do. The probes write settings
// and haptics to the device they select, so a real controller plugged in during
// discovery must never be the match, and a collection whose parent cannot be
// read is rejected rather than guessed. `lower_parent_instance_id` is the
// parent's device instance ID lower-cased by the caller.
[[nodiscard]] constexpr std::optional<unsigned> virtual_sc26_child_slot(
  const std::wstring_view lower_parent_instance_id) noexcept {
  constexpr std::wstring_view prefix = L"vibeshinegamepad";
  const auto amp = lower_parent_instance_id.rfind(L'&');
  if (amp == std::wstring_view::npos) return std::nullopt;
  const std::wstring_view segment = lower_parent_instance_id.substr(amp + 1);
  if (segment.size() <= prefix.size() || segment.substr(0, prefix.size()) != prefix) return std::nullopt;
  // Plain decimal as format_instance_id writes it: no sign, padding or suffix.
  const std::wstring_view digits = segment.substr(prefix.size());
  if (digits.size() > 3 || (digits.size() > 1 && digits[0] == L'0')) return std::nullopt;
  unsigned value = 0;
  for (const wchar_t c : digits) {
    if (c < L'0' || c > L'9') return std::nullopt;
    value = value * 10 + static_cast<unsigned>(c - L'0');
  }
  return value;
}

// True when the collection's parent is the VHF child the driver created for `slot`.
[[nodiscard]] constexpr bool is_virtual_sc26_child_instance(
  const std::wstring_view lower_parent_instance_id, const unsigned slot) noexcept {
  return virtual_sc26_child_slot(lower_parent_instance_id) == slot;
}

}  // namespace lvg
