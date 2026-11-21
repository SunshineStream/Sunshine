/**
 * @file src/platform/macos/misc.h
 * @brief Miscellaneous declarations for macOS platform.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/platform/permissions.h"

namespace platf {
  /**
   * @brief Action to take when a macOS privacy permission is unavailable.
   */
  enum class permission_request_action_t {
    none,  ///< No startup action is needed after an earlier request.
    prompt,  ///< Ask macOS for the native permission.
    settings,  ///< Open the permission's settings pane.
  };

  /**
   * @brief Choose a privacy action without repeating a macOS startup prompt.
   *
   * @param granted Whether the native preflight check reports access.
   * @param previous_state Zero before a request, one after a request, or two after a grant.
   * @param startup Whether this action is part of automatic startup checks.
   * @param retry_manual_request Whether a user-initiated action should retry the native request.
   * @return The action to perform.
   */
  permission_request_action_t permission_request_action(bool granted, int previous_state, bool startup, bool retry_manual_request);

  /**
   * @brief Check whether the macOS version has Local Network privacy controls.
   *
   * @param major_version macOS major version number.
   * @return True on macOS 15 and newer.
   */
  bool supports_local_network_privacy(int major_version);

  /**
   * @brief Decide whether startup should initiate a native permission request.
   *
   * @param permission Permission status and requirement for the active configuration.
   * @param notifications_enabled Whether the system tray uses notifications.
   * @return True when a missing permission should be requested.
   */
  bool should_request_startup_permission(const permission_status_t &permission, bool notifications_enabled);

  /**
   * @brief Request missing permissions needed at startup.
   *
   * @param notifications_enabled Whether the running build shows tray notifications.
   */
  void request_startup_permissions(bool notifications_enabled);

  /**
   * @brief Probe system audio once on startup when no custom sink is configured.
   *
   * The operating system has no passive tap authorization query. Later streams
   * may request access again if the user removes permission.
   */
  void request_startup_system_audio_permission();

  /**
   * @brief Decide whether a one-time startup system audio probe is due.
   *
   * @param has_custom_sink Whether Sunshine uses a custom audio sink.
   * @param previously_requested Whether a startup probe was already attempted.
   * @return True only for the first native system audio startup probe.
   */
  bool should_request_startup_system_audio_permission(bool has_custom_sink, bool previously_requested);

  /**
   * @brief Start and stop an unmuted Core Audio tap to request system audio access.
   *
   * @return True when the temporary tap started successfully.
   */
  bool request_system_audio_permission();

  /**
   * @brief Check whether macOS has granted screen-capture permission.
   *
   * @return True when Sunshine can capture the screen.
   */
  bool is_screen_capture_allowed();

  /**
   * @brief Get the UUID that macOS assigns to a display.
   *
   * Unlike the numeric CoreGraphics display id, the UUID stays the same when a
   * display (including a virtual display) is disconnected and reconnected.
   * @param display_id CoreGraphics display id.
   * @return Upper-case UUID string such as `3F69A49E-CDF5-44A5-B791-62FF4EDB0951`, or empty when unavailable.
   */
  std::string display_uuid(std::uint32_t display_id);

  /**
   * @brief Find the online display with the given UUID.
   *
   * @param uuid Display UUID, compared case-insensitively.
   * @return CoreGraphics display id, or empty when no online display has this UUID.
   */
  std::optional<std::uint32_t> display_id_from_uuid(std::string_view uuid);
}  // namespace platf

namespace dyn {
  typedef void (*apiproc)();

  /**
   * @brief Load persisted state from its backing store.
   *
   * @param handle Native library or object handle used by the operation.
   * @param funcs Function table populated from the loaded library.
   * @param strict Whether missing functions should be treated as an error.
   * @return 0 when all required symbols are loaded; nonzero when loading fails.
   */
  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  /**
   * @brief Return the native handle owned by the wrapper.
   *
   * @param libs List of libraries to probe for the requested symbol.
   * @return Native dynamic-library handle, or nullptr when no library can be opened.
   */
  void *handle(const std::vector<const char *> &libs);

}  // namespace dyn
