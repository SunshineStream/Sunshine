/**
 * @file src/platform/macos/permissions_manager.mm
 * @brief Handles macOS platform permissions.
 */

#include "permissions_manager.h"

#include <Foundation/Foundation.h>

#include "src/logging.h"

namespace platf {
  namespace {
    auto
      screen_capture_allowed = std::atomic { false };
    /**
     * Used to avoid spamming permission requests when the user receives an input event
     */
    bool
      accessibility_permission_requested = std::atomic { false };
    bool
      has_accessibility = std::atomic { false };
  }  // namespace

  // Return whether screen capture is allowed for this process.
  bool
  PermissionsManager::is_screen_capture_allowed() {
    return screen_capture_allowed;
  }

  bool
  PermissionsManager::request_screen_capture_permission() {
    if (!CGPreflightScreenCaptureAccess()) {
      BOOST_LOG(error) << "No screen capture permission!";
      BOOST_LOG(error) << "Please activate it in 'System Preferences' -> 'Privacy' -> 'Screen Recording'";
      CGRequestScreenCaptureAccess();
      return true;
    }
    // Record that we determined that we have the screen capture permission.
    screen_capture_allowed = true;
    return false;
  }

  bool
  PermissionsManager::has_accessibility_permission() {
    NSDictionary *options = @{static_cast<id>(kAXTrustedCheckOptionPrompt): @NO};
    // We use kAXTrustedCheckOptionPrompt == NO here,
    // instead of using XIsProcessTrusted(),
    // because this will update the accessibility list with sunshine current path
    return AXIsProcessTrustedWithOptions(static_cast<CFDictionaryRef>(options));
  }

  bool
  PermissionsManager::has_accessibility_permission_cached() {
    if (has_accessibility) return true;
    if (accessibility_permission_requested) return has_accessibility;
    has_accessibility = has_accessibility_permission();
    return has_accessibility;
  }

  bool
  PermissionsManager::request_accessibility_permission() {
    if (!has_accessibility_permission()) {
      NSDictionary *options = @{static_cast<id>(kAXTrustedCheckOptionPrompt): @YES};
      return !AXIsProcessTrustedWithOptions(static_cast<CFDictionaryRef>(options));
    }
    return false;
  }

  bool
  PermissionsManager::request_accessibility_permission_once() {
    if (!accessibility_permission_requested) {
      accessibility_permission_requested = true;
      return request_accessibility_permission();
    }
    return false;
  }

  void
  PermissionsManager::print_accessibility_status(const bool is_keyboard_event, const bool release) {
    if (!release) return;

    if (!has_accessibility_permission_cached()) {
      request_accessibility_permission_once();
      BOOST_LOG(info) << "Received " << (is_keyboard_event ? "keyboard" : "mouse") << " event but "
                      << default_accessibility_log_msg();
    }
  }

}  // namespace platf
