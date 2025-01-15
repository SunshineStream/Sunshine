/**
 * @file src/platform/macos/permissions_manager.h
 * @brief Handles macOS platform permissions.
 */
#pragma once

#include <Carbon/Carbon.h>

#include <atomic>
#include <string>

namespace platf {
  class PermissionsManager {
  public:
    static std::string
    default_accessibility_log_msg() {
      return "Accessibility permission is not enabled,"
             " please enable sunshine in "
             "[System Settings > Privacy & Security > Privacy > Accessibility]"
             ", then please restart Sunshine for it to take effect";
    }

    PermissionsManager() = default;

    bool
    is_screen_capture_allowed();

    bool
    request_screen_capture_permission();

    /**
     * Checks for Accessibility permission
     * @return returns true if sunshine has Accessibility permission enabled
     */
    bool
    has_accessibility_permission();

    /**
     * Checks for Accessibility permission
     * @return returns true if sunshine has Accessibility permission enabled
     */
    bool
    has_accessibility_permission_cached();

    /**
     * Prompts the user for Accessibility permission
     * @return returns true if requested permission, false if already has permission
     */
    bool
    request_accessibility_permission();

    /**
     * Prompts the user for Accessibility permission
     * @return returns true if requested permission, false if already has permission
     */
    bool
    request_accessibility_permission_once();

    /**
     * Prints the accessibility status based on the input event type and release status
     * @param is_keyboard_event indicates if the event is a keyboard event
     * @param release indicates if the event is a release event
     */
    void
    print_accessibility_status(const bool is_keyboard_event, const bool release);
  };
}  // namespace platf
