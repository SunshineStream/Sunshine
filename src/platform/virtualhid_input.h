/**
 * @file src/platform/virtualhid_input.h
 * @brief Declarations for libvirtualhid-backed input helpers.
 */
#pragma once

// standard includes
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <libvirtualhid/libvirtualhid.hpp>

// local includes
#include "src/platform/common.h"

namespace platf::virtualhid {

  /**
   * @brief Runtime and virtual devices owned by one platform input context.
   */
  struct input_context_t {
    /**
     * @brief Construct the libvirtualhid input context using the platform-default backend.
     */
    input_context_t();

    /**
     * @brief Construct the libvirtualhid input context using a selected backend.
     *
     * @param backend Backend used to create the libvirtualhid runtime.
     */
    explicit input_context_t(lvh::BackendKind backend);

    /**
     * @brief Recreate the shared keyboard using the runtime's current driver and license state.
     */
    void refresh_keyboard();

    /**
     * @brief Recreate the shared mouse using the runtime's current driver and license state.
     */
    void refresh_mouse();

    /**
     * @brief Recreate the shared mouse for a virtual desktop and target viewport.
     *
     * @param desktop Full native virtual-desktop bounds.
     * @param viewport Native bounds of the streamed display.
     */
    void refresh_mouse(const lvh::PointerViewport &desktop, const lvh::PointerViewport &viewport);

    /**
     * @brief Retarget the shared mouse when streamed display geometry changes.
     *
     * @param touch_port Desktop and viewport geometry for pointer input.
     */
    void update_mouse_viewport(const touch_port_t &touch_port);

    std::unique_ptr<lvh::Runtime> runtime;  ///< libvirtualhid runtime.
    std::unique_ptr<lvh::Keyboard> keyboard;  ///< Shared virtual keyboard.
    std::unique_ptr<lvh::Mouse> mouse;  ///< Shared virtual mouse.
    lvh::PointerViewport mouse_desktop;  ///< Virtual-desktop bounds used to create the shared mouse.
    lvh::PointerViewport mouse_viewport;  ///< Streamed display bounds used to create the shared mouse.
    std::vector<std::shared_ptr<struct gamepad_context_t>> gamepads {static_cast<std::size_t>(MAX_GAMEPADS)};  ///< Virtual gamepad slots.
  };

  /**
   * @brief Per-client virtual touch and pen state.
   */
  struct client_context_t {
    /**
     * @brief Create per-client libvirtualhid devices.
     *
     * @param input Global input context.
     */
    explicit client_context_t(input_context_t &input);

    input_context_t *global = nullptr;  ///< Shared global input context.
    std::unique_ptr<lvh::Touchscreen> touch;  ///< Per-client touchscreen.
    std::unique_ptr<lvh::PenTablet> pen;  ///< Per-client pen tablet.
    std::set<std::int32_t> active_touches;  ///< Active touchscreen contacts.
    std::set<lvh::PenButton> pressed_pen_buttons;  ///< Active pen tablet buttons.
  };

  /**
   * @brief Return the shared libvirtualhid context from a platform input backend.
   *
   * @param input Platform input backend.
   * @return Shared libvirtualhid input context.
   */
  input_context_t &get_input_context(input_t &input);

  /**
   * @brief Return the per-client libvirtualhid context from a platform input backend.
   *
   * @param input Per-client platform input backend.
   * @return Per-client libvirtualhid input context.
   */
  client_context_t &get_client_context(client_input_t *input);

  /**
   * @brief Create a platform-default libvirtualhid runtime.
   *
   * @param backend Backend used to create the runtime.
   * @return Runtime instance.
   */
  std::unique_ptr<lvh::Runtime> create_runtime(lvh::BackendKind backend = lvh::BackendKind::platform_default);

  /**
   * @brief Return static gamepad choices for config validation.
   *
   * @return Supported gamepad choices.
   */
  std::vector<supported_gamepad_t> static_supported_gamepads();

  /**
   * @brief Return gamepad choices and runtime availability.
   *
   * @param runtime Runtime to probe.
   * @param fallback_vigem_available Whether Windows ViGEm fallback can create gamepads.
   * @param virtualhid_licensed Whether the broker has a valid license.
   * @param require_license Whether this platform requires a broker license for gamepads.
   * @return Supported gamepad choices.
   */
  std::vector<supported_gamepad_t> supported_gamepads(
    lvh::Runtime *runtime,
    bool fallback_vigem_available = false,
    bool virtualhid_licensed = true,
    bool require_license = false
  );

  /**
   * @brief Decide whether a libvirtualhid runtime may create a gamepad.
   *
   * @param capabilities Runtime backend capabilities.
   * @param gamepad_driver Configured Windows virtual gamepad driver policy.
   * @param virtualhid_licensed Whether the Virtual HID Driver machine license is valid.
   * @return True when the libvirtualhid runtime should receive gamepad allocations.
   */
  bool should_use_gamepad_runtime(
    const lvh::BackendCapabilities &capabilities,
    std::string_view gamepad_driver,
    bool virtualhid_licensed
  );

  /**
   * @brief Decide whether ViGEmBus should be tried for a gamepad allocation.
   *
   * When Virtual HID Driver is unavailable or deliberately bypassed, ViGEmBus
   * uses automatic selection for profiles it cannot represent directly.
   *
   * @param configured_gamepad Configured virtual gamepad profile.
   * @param virtualhid_selected Whether Virtual HID Driver was selected for the allocation.
   * @param gamepad_driver Configured Windows virtual gamepad driver policy.
   * @return True when Sunshine should attempt the ViGEmBus fallback.
   */
  bool should_try_vigembus_fallback(
    std::string_view configured_gamepad,
    bool virtualhid_selected,
    std::string_view gamepad_driver
  );

  /**
   * @brief Allocate a libvirtualhid gamepad.
   *
   * @param context Input context.
   * @param id Sunshine gamepad identifiers.
   * @param metadata Client-reported controller metadata.
   * @param feedback_queue Queue used to return gamepad feedback to the client.
   * @return 0 on success, otherwise -1.
   */
  int alloc_gamepad(input_context_t &context, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);

  /**
   * @brief Rebind an existing libvirtualhid gamepad to a resumed client session.
   *
   * @param context Input context.
   * @param id Sunshine gamepad identifiers for the resumed client.
   * @param feedback_queue Queue used to return gamepad feedback to the resumed client.
   * @return 0 when the gamepad exists and was rebound; otherwise -1.
   */
  int rebind_gamepad(input_context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue);

  /**
   * @brief Check whether a libvirtualhid gamepad exists in a slot.
   *
   * @param context Input context.
   * @param nr Gamepad slot index.
   * @return True when a virtual gamepad is active.
   */
  bool has_gamepad(const input_context_t &context, int nr);

#ifdef SUNSHINE_TESTS
  /**
   * @brief Return the adapter allocated in a gamepad slot for unit testing.
   *
   * @param context Input context.
   * @param nr Gamepad slot index.
   * @return Allocated adapter, or `nullptr` when the slot is inactive.
   */
  lvh::GamepadStateAdapter *gamepad_adapter_for_testing(input_context_t &context, int nr);
#endif

  /**
   * @brief Release a libvirtualhid gamepad slot.
   *
   * @param context Input context.
   * @param nr Gamepad slot index.
   */
  void free_gamepad(input_context_t &context, int nr);

  /**
   * @brief Submit a full gamepad state.
   *
   * @param context Input context.
   * @param nr Gamepad slot index.
   * @param state Sunshine gamepad state.
   */
  void gamepad_update(input_context_t &context, int nr, const gamepad_state_t &state);

  /**
   * @brief Submit a gamepad touch event.
   *
   * @param context Input context.
   * @param touch Sunshine touch event.
   */
  void gamepad_touch(input_context_t &context, const gamepad_touch_t &touch);

  /**
   * @brief Submit a gamepad motion event.
   *
   * @param context Input context.
   * @param motion Sunshine motion event.
   */
  void gamepad_motion(input_context_t &context, const gamepad_motion_t &motion);

  /**
   * @brief Submit gamepad battery metadata.
   *
   * @param context Input context.
   * @param battery Sunshine battery event.
   */
  void gamepad_battery(input_context_t &context, const gamepad_battery_t &battery);

  /**
   * @brief Move the virtual mouse relatively.
   *
   * @param context Input context.
   * @param delta_x Horizontal delta.
   * @param delta_y Vertical delta.
   */
  void move_mouse(input_context_t &context, int delta_x, int delta_y);

  /**
   * @brief Retarget the virtual mouse and move it relatively.
   *
   * @param context Input context.
   * @param touch_port Desktop and streamed-display bounds for the pointer.
   * @param delta_x Horizontal delta.
   * @param delta_y Vertical delta.
   */
  void move_mouse(input_context_t &context, const touch_port_t &touch_port, int delta_x, int delta_y);

  /**
   * @brief Move the virtual mouse absolutely inside a target touch port.
   *
   * @param context Input context.
   * @param touch_port Target coordinate space.
   * @param x Absolute X coordinate.
   * @param y Absolute Y coordinate.
   */
  void abs_mouse(input_context_t &context, const touch_port_t &touch_port, float x, float y);

  /**
   * @brief Submit a mouse button event.
   *
   * @param context Input context.
   * @param button Moonlight mouse button.
   * @param release Whether the button was released.
   */
  void button_mouse(input_context_t &context, int button, bool release);

  /**
   * @brief Submit vertical scroll input.
   *
   * @param context Input context.
   * @param high_res_distance High-resolution scroll distance.
   */
  void scroll(input_context_t &context, int high_res_distance);

  /**
   * @brief Submit horizontal scroll input.
   *
   * @param context Input context.
   * @param high_res_distance High-resolution scroll distance.
   */
  void hscroll(input_context_t &context, int high_res_distance);

  /**
   * @brief Submit a keyboard key transition.
   *
   * @param context Input context.
   * @param modcode Portable key code.
   * @param release Whether the key was released.
   * @param flags Bit flags that modify the requested operation.
   * @param extended Whether the client positively identified an extended key.
   */
  void keyboard_update(input_context_t &context, std::uint16_t modcode, bool release, std::uint8_t flags, bool extended = false);

  /**
   * @brief Submit UTF-8 text input.
   *
   * @param context Input context.
   * @param utf8 UTF-8 text buffer.
   * @param size Text buffer size.
   */
  void unicode(input_context_t &context, const char *utf8, int size);

  /**
   * @brief Submit a touchscreen event.
   *
   * @param context Client context.
   * @param touch_port Touch coordinate bounds used for scaling.
   * @param touch Touch event.
   */
  void touch_update(client_context_t &context, const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief Submit a pen event.
   *
   * @param context Client context.
   * @param touch_port Touch coordinate bounds used for scaling.
   * @param pen Pen event.
   */
  void pen_update(client_context_t &context, const touch_port_t &touch_port, const pen_input_t &pen);

  /**
   * @brief Return whether the configured gamepad profile can expose touchpad input.
   *
   * @return True when controller touchpad input should be advertised.
   */
  bool configured_gamepad_supports_touchpad();

  /**
   * @brief Return whether the configured gamepad profile needs Moonlight controller extensions.
   *
   * Moonlight uses the controller-touch feature flag to authorize both controller
   * touchpad and motion packets.
   *
   * @return True when controller touchpad or motion input should be advertised.
   */
  bool configured_gamepad_supports_controller_extensions();

  /**
   * @brief Determine the logind seat targeted by this Sunshine instance.
   *
   * @return Value of `XDG_SEAT`, or an empty string when it is unset.
   */
  std::string target_seat();

  /**
   * @brief Append a seat suffix to a virtual device name for multi-seat udev rules.
   *
   * @param base_name Device name advertised by the libvirtualhid profile.
   * @param seat Target logind seat.
   * @return `base_name` unchanged for an empty seat or `seat0`, otherwise `"<base_name> (<seat>)"`.
   */
  std::string device_name_for_seat(std::string_view base_name, std::string_view seat);

}  // namespace platf::virtualhid
