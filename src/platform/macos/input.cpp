/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for libvirtualhid-backed macOS input handling.
 */

// platform includes
#include <ApplicationServices/ApplicationServices.h>

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// lib includes
#include <libvirtualhid/license.hpp>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/macos/mouse_utils.h"
#include "src/platform/virtualhid_input.h"

namespace platf {
  namespace {
    constexpr uint32_t max_displays = 16;

    /**
     * @brief Read the pointer position in global CoreGraphics point space.
     * @return The current pointer location, or an empty optional when events are unavailable.
     */
    std::optional<CGPoint> cursor_point() {
      const auto event = CGEventCreate(nullptr);
      if (!event) {
        return std::nullopt;
      }

      const auto point = CGEventGetLocation(event);
      CFRelease(event);
      return point;
    }

    /**
     * @brief Collect the bounds of every active display.
     * @return The union of the display layout, or an empty optional if it is unavailable.
     */
    std::optional<macos::mouse::bounds_t> active_layout_bounds() {
      uint32_t count = 0;
      if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0) {
        return std::nullopt;
      }

      CGDirectDisplayID displays[max_displays];
      count = std::min(count, max_displays);
      if (CGGetActiveDisplayList(count, displays, &count) != kCGErrorSuccess) {
        return std::nullopt;
      }

      std::vector<macos::mouse::rect_t> rects;
      rects.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        const auto bounds = CGDisplayBounds(displays[i]);
        rects.push_back({bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height});
      }

      return macos::mouse::layout_bounds(rects);
    }
  }  // namespace

  std::optional<util::point_t> get_mouse_loc(input_t & /*input*/) {
    const auto current = cursor_point();
    if (!current) {
      return std::nullopt;
    }

    return util::point_t {current->x, current->y};
  }

  namespace macos {
    bool move_mouse_relative(int delta_x, int delta_y) {
      // CoreGraphics discards posted events without Accessibility. Report that so the caller
      // can use the virtual HID path instead of silently dropping the movement.
      if (!AXIsProcessTrusted()) {
        return false;
      }

      static const bool logged = [] {
        BOOST_LOG(info) << "macOS relative mouse backend selected: CoreGraphics";
        return true;
      }();
      (void) logged;

      static std::optional<mouse::bounds_t> layout;  // Only reached from the input thread.
      static std::optional<CGPoint> predicted;
      static std::chrono::steady_clock::time_point last_packet;

      // CGEventGetLocation() reports only what the window server has committed so far. Re-basing
      // every packet on it makes a burst at 500-1000 Hz read the same stale position repeatedly,
      // so each packet overwrites the previous one's movement and the pointer under-travels.
      // Accumulate the chain locally instead, and trust the server again on the first packet or
      // after a gap long enough for the pointer to have been moved by something else.
      const auto now = std::chrono::steady_clock::now();
      auto base = predicted;
      if (!base || now - last_packet > std::chrono::milliseconds {100}) {
        const auto live = cursor_point();
        if (!live) {
          return false;
        }

        base = live;
      }

      auto target = CGPoint {base->x + delta_x, base->y + delta_y};
      if (!layout || target.x < layout->min_x || target.y < layout->min_y || target.x >= layout->max_x || target.y >= layout->max_y) {
        const auto refreshed = active_layout_bounds();
        if (!refreshed) {
          return false;
        }

        layout = refreshed;
        target = mouse::clamp_to_layout(target, *layout);
      }

      const auto type = mouse::move_event_type(
        CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonLeft),
        CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonRight),
        CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonCenter)
      );

      const auto event = CGEventCreateMouseEvent(nullptr, type, target, mouse::move_event_button(type));
      if (!event) {
        return false;
      }

      // Describe the movement once, with the location and the device deltas agreeing: a fresh
      // event leaves the deltas at zero, so apps reading them instead of the cursor location
      // would see no movement at all.
      CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, target.x - base->x);
      CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, target.y - base->y);

      // Modifiers are reported per event, so a Shift/Option/Ctrl held during a drag has to travel
      // with the motion. The button press comes from the virtual HID backend, which stamps the
      // same live state onto its mouse events; without this the press carries the modifier and
      // every drag after it loses it.
      CGEventSetFlags(event, CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState));

      // Exactly one pointer update: no CGWarpMouseCursorPosition() on top of this event.
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);

      predicted = target;
      last_packet = now;
      return true;
    }
  }  // namespace macos

  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;
    const auto runtime = virtualhid::create_runtime();
    if (!runtime) {
      return caps;
    }

    const auto &capabilities = runtime->capabilities();
    if (config::input.gamepad_driver != config::GAMEPAD_DRIVER_NONE && capabilities.supports_gamepad && lvh::get_license_status().license.licensed() && virtualhid::configured_gamepad_supports_controller_extensions()) {
      caps |= platform_caps::controller_touch;
    }
    if (config::input.native_pen_touch && (capabilities.supports_touchscreen || capabilities.supports_pen_tablet)) {
      caps |= platform_caps::pen_touch;
    }

    return caps;
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector<supported_gamepad_t> gamepads;
    if (!input || !input->get()) {
      gamepads = virtualhid::static_supported_gamepads();
      return gamepads;
    }

    if (config::input.gamepad_driver == config::GAMEPAD_DRIVER_NONE) {
      gamepads.clear();
      return gamepads;
    }

    const auto licensed = lvh::get_license_status().license.licensed();
    gamepads = virtualhid::supported_gamepads(virtualhid::get_input_context(*input).runtime.get(), false, licensed, true);
    return gamepads;
  }

}  // namespace platf
