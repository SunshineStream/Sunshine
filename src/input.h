/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param input Raw input packet to format for logging.
   */
  void print(void *input);
  /**
   * @brief Reset stream input state after a client disconnect or shutdown.
   *
   * @param input Shared stream input state to reset.
   */
  void reset(std::shared_ptr<input_t> &input);

  /**
   * @brief Destroy every retained virtual gamepad session.
   *
   * Retained gamepads survive a paused transport connection so they can be reused on resume. Call this when the
   * streamed application or all streaming sessions are explicitly terminated.
   */
  void terminate_gamepads();

  /**
   * @brief Destroy virtual gamepads retained for one paired client.
   *
   * @param session_id Stable paired-client identity used by alloc().
   */
  void terminate_gamepads(std::string_view session_id);

  /**
   * @brief Queue a raw input message for platform passthrough.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, std::uint64_t focus_session = 0);

  /**
   * @brief Initialize global input resources and platform backends.
   *
   * @return Cleanup handle for initialized input resources, or null if none are required.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Probe whether the platform can create virtual gamepads.
   *
   * @return True when at least one configured gamepad backend is available.
   */
  bool probe_gamepads();

  /**
   * @brief Recreate shared libvirtualhid keyboard and mouse devices after a license-state change.
   *
   * The work is serialized with streamed input so both backends can switch
   * safely between the Windows HID and SendInput paths.
   */
  void refresh_virtual_input();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @param session_id Stable paired-client identity shared by launch and resume connections.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string session_id);

#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Replace the global platform input backend for a unit test.
     *
     * @param input Test-owned platform input backend.
     */
    void set_platform_input(platf::input_t input);

    /**
     * @brief Access a retained stream's platform client devices in a unit test.
     *
     * @param input Retained input state.
     * @return Borrowed platform client input context.
     */
    platf::client_input_t *client_context(const std::shared_ptr<input_t> &input);

    /**
     * @brief Allocate a gamepad directly in retained input state for a unit test.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @param metadata Client-reported controller metadata.
     * @return Assigned global gamepad slot, or -1 on failure.
     */
    int alloc_gamepad(std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_arrival_t &metadata);

    /**
     * @brief Return the global gamepad slot stored for a test controller.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @return Assigned global gamepad slot, or -1 when unallocated.
     */
    int gamepad_id(const std::shared_ptr<input_t> &input, std::uint8_t client_index);

    /**
     * @brief Install a callback called just before dispatching a queued input packet in a test.
     * @param hook Callback receiving the packet magic, or empty to remove it.
     */
    void set_input_packet_hook(std::function<void(std::uint32_t)> hook);

    /**
     * @brief Redirect input task scheduling to a test callback.
     *
     * @param sink Callback receiving the stream whose packet task was queued, or empty to restore the worker pool.
     */
    void set_input_task_sink(std::function<void(std::shared_ptr<input_t>)> sink);

    /**
     * @brief Process queued input packets on the calling thread in a test.
     *
     * @param input Retained stream input state.
     */
    void process_queued_messages(std::shared_ptr<input_t> input);

    /**
     * @brief Keyboard event Sunshine emitted toward the platform backend.
     */
    struct keyboard_event_t {
      std::uint16_t key_code;  ///< Platform keycode after the configured keybinding remap.
      bool release;  ///< Whether the event releases the key.
      std::uint8_t flags;  ///< Bit flags carried by the client keyboard packet.
      bool extended = false;  ///< Whether the client positively identified an extended key.
    };

    /**
     * @brief Redirect keyboard output away from the host operating system.
     *
     * Tests must install a sink before emitting keys, otherwise the events are typed into the
     * machine running the test suite.
     *
     * @param sink Recorder invoked in place of platf::keyboard_update, or empty to restore
     *             delivery to the platform backend.
     */
    void set_keyboard_sink(std::function<void(const keyboard_event_t &)> sink);

    /**
     * @brief Process one client keyboard packet on the calling thread.
     *
     * @param input Retained input state.
     * @param key_code Windows virtual-key code sent by the client.
     * @param modifiers Client modifier bitmask carried by the packet.
     * @param flags Bit flags carried by the client keyboard packet.
     * @param release Whether the packet releases the key.
     */
    void send_keyboard_packet(std::shared_ptr<input_t> &input, std::uint16_t key_code, std::uint8_t modifiers, std::uint8_t flags, bool release);

    /**
     * @brief Process one client relative-mouse packet on the calling thread.
     *
     * @param input Retained input state.
     * @param delta_x Horizontal client mouse delta.
     * @param delta_y Vertical client mouse delta.
     */
    void send_relative_mouse_packet(std::shared_ptr<input_t> &input, std::int16_t delta_x, std::int16_t delta_y);

    /**
     * @brief Process one client absolute-mouse packet on the calling thread.
     *
     * @param input Retained input state.
     * @param x Horizontal position on the client surface.
     * @param y Vertical position on the client surface.
     * @param width Width of the client surface.
     * @param height Height of the client surface.
     */
    void send_absolute_mouse_packet(std::shared_ptr<input_t> &input, std::int16_t x, std::int16_t y, std::int16_t width, std::int16_t height);

    /**
     * @brief Forget every key Sunshine tracks as pressed and cancel any pending key repeat.
     */
    void reset_keyboard_state();

    /**
     * @brief Release every key Sunshine tracks as pressed, as a disconnect does.
     */
    void release_held_keys();

    /**
     * @brief Validate raw protocol input bytes for a unit test.
     *
     * @param packet Raw packet bytes.
     * @return True when the packet is safe for typed processing.
     */
    bool is_valid_input_packet(std::span<const std::uint8_t> packet);

    /**
     * @brief Return the number of validated packets waiting in a test input queue.
     *
     * @param input Shared stream input state.
     * @return Number of queued packets, or zero for an empty input pointer.
     */
    std::size_t queued_input_packet_count(const std::shared_ptr<input_t> &input);
  }  // namespace testing
#endif

  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;  ///< Width of the full capture environment in physical pixels.
    int env_height;  ///< Height of the full capture environment in physical pixels.

    // Offset x and y coordinates of the client
    float client_offsetX;  ///< Horizontal client viewport offset used when scaling touch input.
    float client_offsetY;  ///< Vertical client viewport offset used when scaling touch input.

    float scalar_inv;  ///< Inverse scale factor from client coordinates to display coordinates.
    float scalar_tpcoords;  ///< Scale factor from client coordinates to touch-port coordinates.

    int env_logical_width;  ///< Width of the full capture environment after display scaling.
    int env_logical_height;  ///< Height of the full capture environment after display scaling.
    int capture_left {}, capture_top {};  ///< Windows global physical display origin.
    int capture_width {}, capture_height {};  ///< Physical pixel extent of the captured display.

    /**
     * @brief Check whether the touch-port bounds are initialized.
     */
    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
