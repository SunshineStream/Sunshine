/**
 * @file src/platform/virtualhid_input.cpp
 * @brief Definitions for libvirtualhid-backed input helpers.
 */

// standard includes
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <string_view>
#include <utility>

// lib includes
#include <lizardbyte/common/env.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "virtualhid_input.h"

using namespace std::literals;

namespace platf::virtualhid {
  /**
   * @brief Runtime state for one virtual gamepad.
   */
  struct gamepad_context_t {
    std::unique_ptr<lvh::GamepadStateAdapter> adapter;  ///< State adapter for the virtual gamepad.
    std::mutex feedback_mutex;  ///< Synchronizes feedback routing with session rebinding.
    feedback_queue_t feedback_queue;  ///< Feedback queue for client output events.
    std::array<std::optional<std::uint32_t>, 2> touch_ids;  ///< Client touch IDs assigned to libvirtualhid slots.
    std::uint8_t client_relative_index = 0;  ///< Client-relative controller index.
    bool has_last_rumble = false;  ///< Whether last rumble values are valid.
    std::uint16_t last_low_frequency_rumble = 0;  ///< Last low-frequency rumble value.
    std::uint16_t last_high_frequency_rumble = 0;  ///< Last high-frequency rumble value.
    bool has_last_trigger_rumble = false;  ///< Whether last trigger rumble values are valid.
    std::uint16_t last_left_trigger_rumble = 0;  ///< Last left trigger rumble value.
    std::uint16_t last_right_trigger_rumble = 0;  ///< Last right trigger rumble value.
    bool has_last_rgb = false;  ///< Whether last RGB values are valid.
    std::uint8_t last_red = 0;  ///< Last red LED value.
    std::uint8_t last_green = 0;  ///< Last green LED value.
    std::uint8_t last_blue = 0;  ///< Last blue LED value.
    bool has_last_player_leds = false;  ///< Whether last player indicator LED values are valid.
    std::uint8_t last_solid_player_leds = 0;  ///< Last solid player indicator mask.
    std::uint8_t last_flashing_player_leds = 0;  ///< Last flashing player indicator mask.
  };

  namespace {

    /**
     * @brief Gamepad profile exposed through Sunshine config.
     */
    struct gamepad_profile_t {
      std::string_view name;  ///< Sunshine config value.
      lvh::GamepadProfileKind kind;  ///< libvirtualhid profile kind.
      lvh::DeviceProfile (*profile)();  ///< Profile factory.
    };

    /**
     * @brief Supported libvirtualhid gamepad profiles.
     */
    constexpr std::array gamepad_profiles {
      gamepad_profile_t {"generic", lvh::GamepadProfileKind::generic, lvh::profiles::generic_gamepad},
      gamepad_profile_t {"x360", lvh::GamepadProfileKind::xbox_360, lvh::profiles::xbox_360},
      gamepad_profile_t {"xone", lvh::GamepadProfileKind::xbox_one, lvh::profiles::xbox_one},
      gamepad_profile_t {"xseries", lvh::GamepadProfileKind::xbox_series, lvh::profiles::xbox_series},
      gamepad_profile_t {"ds4", lvh::GamepadProfileKind::dualshock4, lvh::profiles::dualshock4},
      gamepad_profile_t {"ds5", lvh::GamepadProfileKind::dualsense, lvh::profiles::dualsense},
      gamepad_profile_t {"switch", lvh::GamepadProfileKind::switch_pro, lvh::profiles::switch_pro},
    };

    void log_failure(std::string_view operation, const lvh::OperationStatus &status) {
      if (!status.ok()) {
        BOOST_LOG(warning) << operation << ": "sv << status.message();
      }
    }

    /**
     * @brief Forward one libvirtualhid diagnostic to Sunshine's logger.
     *
     * @param level libvirtualhid diagnostic severity.
     * @param message Diagnostic message.
     */
    void log_virtualhid_message(lvh::LogLevel level, const std::string &message) {
      switch (level) {
        case lvh::LogLevel::debug:
          BOOST_LOG(debug) << "[libvirtualhid] "sv << message;
          break;
        case lvh::LogLevel::info:
          BOOST_LOG(info) << "[libvirtualhid] "sv << message;
          break;
        case lvh::LogLevel::warning:
          BOOST_LOG(warning) << "[libvirtualhid] "sv << message;
          break;
        case lvh::LogLevel::error:
          BOOST_LOG(error) << "[libvirtualhid] "sv << message;
          break;
      }
    }

    /**
     * @brief Scope a libvirtualhid profile name to the target seat.
     *
     * @param profile Profile whose advertised device name is updated.
     */
    void apply_target_seat(lvh::DeviceProfile &profile) {
      profile.name = device_name_for_seat(profile.name, target_seat());
    }

    /**
     * @brief Compare two pointer viewports.
     *
     * @param left First viewport.
     * @param right Second viewport.
     * @return `true` when every bound matches.
     */
    bool pointer_viewports_equal(const lvh::PointerViewport &left, const lvh::PointerViewport &right) {
      return left.offset_x == right.offset_x &&
             left.offset_y == right.offset_y &&
             left.width == right.width &&
             left.height == right.height;
    }

    /**
     * @brief Convert Sunshine absolute-input geometry to libvirtualhid bounds.
     *
     * @param touch_port Sunshine desktop and display geometry.
     * @return Full virtual-desktop bounds followed by the streamed viewport.
     */
    std::pair<lvh::PointerViewport, lvh::PointerViewport> pointer_viewports(const touch_port_t &touch_port) {
      const lvh::PointerViewport desktop {
        .offset_x = touch_port.env_offset_x,
        .offset_y = touch_port.env_offset_y,
        .width = touch_port.width,
        .height = touch_port.height,
      };

      if (touch_port.logical_width <= 0 || touch_port.logical_height <= 0) {
        return {desktop, desktop};
      }

      return {
        desktop,
        {
          .offset_x = touch_port.offset_x,
          .offset_y = touch_port.offset_y,
          .width = touch_port.logical_width,
          .height = touch_port.logical_height,
        },
      };
    }

    float normalize_axis(std::int16_t value) {
      if (value < 0) {
        return std::max(-1.0F, static_cast<float>(value) / 32768.0F);
      }

      return std::min(1.0F, static_cast<float>(value) / 32767.0F);
    }

    float normalize_trigger(std::uint8_t value) {
      return static_cast<float>(value) / static_cast<float>(std::numeric_limits<std::uint8_t>::max());
    }

    lvh::ClientControllerType client_controller_type(std::uint8_t type) {
      using enum lvh::ClientControllerType;

      switch (type) {
        case LI_CTYPE_XBOX:
          return xbox;
        case LI_CTYPE_PS:
          return playstation;
        case LI_CTYPE_NINTENDO:
          return nintendo;
        case LI_CTYPE_UNKNOWN:
        default:
          return unknown;
      }
    }

    const gamepad_profile_t &profile_for_name(std::string_view name) {
      if (const auto iter = std::ranges::find(gamepad_profiles, name, &gamepad_profile_t::name); iter != gamepad_profiles.end()) {
        return *iter;
      }

      return gamepad_profiles[3];  // Xbox Series.
    }

    const gamepad_profile_t &profile_for_metadata(const gamepad_arrival_t &metadata) {
      if (config::input.gamepad != "auto"sv) {
        return profile_for_name(config::input.gamepad);
      }

      if (metadata.type == LI_CTYPE_PS) {
        BOOST_LOG(info) << "Gamepad will be DualSense controller (auto-selected by client-reported type)"sv;
        return profile_for_name("ds5"sv);
      }
      if (metadata.type == LI_CTYPE_NINTENDO) {
        BOOST_LOG(info) << "Gamepad will be Nintendo Switch Pro controller (auto-selected by client-reported type)"sv;
        return profile_for_name("switch"sv);
      }
      if (metadata.type == LI_CTYPE_XBOX) {
        BOOST_LOG(info) << "Gamepad will be Xbox Series controller (auto-selected by client-reported type)"sv;
        return profile_for_name("xseries"sv);
      }
      if (config::input.motion_as_ds4 && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
        BOOST_LOG(info) << "Gamepad will be DualSense controller (auto-selected by motion sensor presence)"sv;
        return profile_for_name("ds5"sv);
      }
      if (config::input.touchpad_as_ds4 && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
        BOOST_LOG(info) << "Gamepad will be DualSense controller (auto-selected by touchpad presence)"sv;
        return profile_for_name("ds5"sv);
      }

      BOOST_LOG(info) << "Gamepad will be Xbox Series controller (default)"sv;
      return profile_for_name("xseries"sv);
    }

    std::string random_private_mac() {
      std::random_device random;
      return std::format(
        "02:00:{:02x}:{:02x}:{:02x}:{:02x}",
        random() & 0xFF,
        random() & 0xFF,
        random() & 0xFF,
        random() & 0xFF
      );
    }

    std::string gamepad_stable_id(const gamepad_id_t &id, const lvh::DeviceProfile &profile) {
      if (profile.gamepad_kind != lvh::GamepadProfileKind::dualshock4 && profile.gamepad_kind != lvh::GamepadProfileKind::dualsense) {
        return std::format("sunshine-gamepad-{}", id.globalIndex);
      }

      if (config::input.virtualhid_randomize_mac || id.globalIndex < 0 || id.globalIndex > 255) {
        return random_private_mac();
      }

      return std::format("02:00:00:00:00:{:02x}", id.globalIndex);
    }

    lvh::GamepadMetadata gamepad_metadata(const gamepad_id_t &id, const gamepad_arrival_t &metadata, const lvh::DeviceProfile &profile) {
      lvh::GamepadMetadata result;
      result.global_index = id.globalIndex;
      result.client_relative_index = id.clientRelativeIndex;
      result.client_type = client_controller_type(metadata.type);
      result.has_motion_sensors = metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO);
      result.has_touchpad = metadata.capabilities & LI_CCAP_TOUCHPAD;
      result.has_rgb_led = metadata.capabilities & LI_CCAP_RGB_LED;
      result.has_battery = metadata.capabilities & LI_CCAP_BATTERY_STATE;
      result.stable_id = gamepad_stable_id(id, profile);
      return result;
    }

    void warn_unsupported_client_features(int global_index, const gamepad_arrival_t &metadata, const lvh::GamepadProfileSupport &support) {
      if ((metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO)) && !support.supports_motion) {
        BOOST_LOG(warning) << "Gamepad "sv << global_index << " has motion sensors, but the selected virtual profile cannot expose them"sv;
      }
      if ((metadata.capabilities & LI_CCAP_TOUCHPAD) && !support.supports_touchpad) {
        BOOST_LOG(warning) << "Gamepad "sv << global_index << " has a touchpad, but the selected virtual profile cannot expose it"sv;
      }
      if ((metadata.capabilities & LI_CCAP_RGB_LED) && !support.supports_rgb_led) {
        BOOST_LOG(warning) << "Gamepad "sv << global_index << " has an RGB LED, but the selected virtual profile cannot expose it"sv;
      }
    }

    void warn_missing_client_features(int global_index, const gamepad_arrival_t &metadata, const lvh::GamepadProfileSupport &support) {
      if (support.supports_motion && !(metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
        BOOST_LOG(warning) << "Gamepad "sv << global_index << " is emulating a motion-capable controller, but the client gamepad does not have motion sensors active"sv;
      }
      if (support.supports_touchpad && !(metadata.capabilities & LI_CCAP_TOUCHPAD)) {
        BOOST_LOG(warning) << "Gamepad "sv << global_index << " is emulating a touchpad-capable controller, but the client gamepad does not have a touchpad"sv;
      }
    }

    lvh::GamepadState make_gamepad_state(const gamepad_state_t &state, const lvh::GamepadProfileSupport &support) {
      using enum lvh::GamepadButton;

      lvh::GamepadState result;
      const auto flags = state.buttonFlags;

      result.buttons.set(dpad_up, flags & DPAD_UP);
      result.buttons.set(dpad_down, flags & DPAD_DOWN);
      result.buttons.set(dpad_left, flags & DPAD_LEFT);
      result.buttons.set(dpad_right, flags & DPAD_RIGHT);
      result.buttons.set(start, flags & START);
      result.buttons.set(back, flags & BACK);
      result.buttons.set(left_stick, flags & LEFT_STICK);
      result.buttons.set(right_stick, flags & RIGHT_STICK);
      result.buttons.set(left_shoulder, flags & LEFT_BUTTON);
      result.buttons.set(right_shoulder, flags & RIGHT_BUTTON);
      result.buttons.set(guide, flags & HOME);
      result.buttons.set(a, flags & A);
      result.buttons.set(b, flags & B);
      result.buttons.set(x, flags & X);
      result.buttons.set(y, flags & Y);
      result.buttons.set(misc1, support.supports_misc1_button && (flags & MISC_BUTTON));
      result.buttons.set(touchpad, support.supports_touchpad_button && (flags & TOUCHPAD_BUTTON));

      if (support.supports_touchpad_button && config::input.ds4_back_as_touchpad_click && configured_gamepad_supports_touchpad() && (flags & BACK)) {
        result.buttons.set(touchpad);
      }

      result.left_stick = {.x = normalize_axis(state.lsX), .y = normalize_axis(state.lsY)};
      result.right_stick = {.x = normalize_axis(state.rsX), .y = normalize_axis(state.rsY)};
      result.left_trigger = normalize_trigger(state.lt);
      result.right_trigger = normalize_trigger(state.rt);
      return result;
    }

    std::optional<lvh::MouseButton> mouse_button(int button) {
      using enum lvh::MouseButton;

      switch (button) {
        case BUTTON_LEFT:
          return left;
        case BUTTON_MIDDLE:
          return middle;
        case BUTTON_RIGHT:
          return right;
        case BUTTON_X1:
          return side;
        case BUTTON_X2:
          return extra;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: "sv << button;
          return std::nullopt;
      }
    }

    lvh::GamepadBatteryState battery_state(std::uint8_t state) {
      using enum lvh::GamepadBatteryState;

      switch (state) {
        case LI_BATTERY_STATE_DISCHARGING:
          return discharging;
        case LI_BATTERY_STATE_CHARGING:
          return charging;
        case LI_BATTERY_STATE_FULL:
          return full;
        case LI_BATTERY_STATE_NOT_PRESENT:
        case LI_BATTERY_STATE_NOT_CHARGING:
          return charging_error;
        case LI_BATTERY_STATE_UNKNOWN:
        default:
          return unknown;
      }
    }

    std::int32_t touch_orientation(std::uint16_t rotation) {
      if (rotation == LI_ROT_UNKNOWN) {
        return 0;
      }

      auto adjusted = static_cast<int>(rotation);
      if (adjusted > 90 && adjusted < 270) {
        adjusted = 180 - adjusted;
      }
      if (adjusted > 90) {
        adjusted -= 360;
      } else if (adjusted < -90) {
        adjusted += 360;
      }

      return adjusted;
    }

    lvh::PointerViewport pointer_viewport(const touch_port_t &touch_port) {
      return {
        .offset_x = touch_port.offset_x,
        .offset_y = touch_port.offset_y,
        .width = touch_port.width,
        .height = touch_port.height,
        .output_name = touch_port.output_name,
      };
    }

    lvh::KeyboardEvent keyboard_event(std::uint16_t modcode, bool release, std::uint8_t flags, bool extended) {
      lvh::KeyboardEvent event {
        .key_code = modcode,
        .pressed = !release,
        .extended = extended,
      };

#ifdef _WIN32
      event.uses_normalized_key_code = (static_cast<std::byte>(flags) & static_cast<std::byte>(SS_KBE_FLAG_NON_NORMALIZED)) == std::byte {};
      event.prefer_native_scan_code = config::input.always_send_scancodes;
#else
      event.uses_normalized_key_code = (static_cast<std::byte>(flags) & static_cast<std::byte>(SS_KBE_FLAG_NON_NORMALIZED)) == std::byte {};
#endif
      return event;
    }

    /**
     * @brief Pack four player indicator states into a protocol bit mask.
     *
     * @param leds Player indicator states ordered from player one through four.
     * @return Four-bit player indicator mask.
     */
    std::uint8_t player_led_mask(const std::array<bool, 4> &leds) {
      std::byte mask {};
      for (std::size_t index = 0; index < leds.size(); ++index) {
        if (leds[index]) {
          mask |= std::byte {1} << index;
        }
      }
      return std::to_integer<std::uint8_t>(mask);
    }

    lvh::PenToolType pen_tool(std::uint8_t tool) {
      using enum lvh::PenToolType;

      switch (tool) {
        case LI_TOOL_TYPE_PEN:
          return pen;
        case LI_TOOL_TYPE_ERASER:
          return eraser;
        case LI_TOOL_TYPE_UNKNOWN:
        default:
          return unchanged;
      }
    }

    /**
     * @brief Raise gamepad feedback while the caller owns the feedback lock.
     *
     * @param gamepad Gamepad receiving output from the virtual device.
     * @param message Feedback message to deliver to the client.
     */
    void raise_feedback_unlocked(const std::shared_ptr<gamepad_context_t> &gamepad, const gamepad_feedback_msg_t &message) {
      if (gamepad->feedback_queue) {
        gamepad->feedback_queue->raise(message);
      }
    }

    /**
     * @brief Raise gamepad feedback using the current client routing.
     *
     * @param gamepad Gamepad receiving output from the virtual device.
     * @param message Feedback message to deliver to the client.
     */
    void raise_feedback(const std::shared_ptr<gamepad_context_t> &gamepad, const gamepad_feedback_msg_t &message) {
      std::lock_guard lock {gamepad->feedback_mutex};
      raise_feedback_unlocked(gamepad, message);
    }

    /**
     * @brief Translate a libvirtualhid output event into Moonlight feedback.
     *
     * @param gamepad Gamepad that emitted the output event.
     * @param output Output event reported by libvirtualhid.
     */
    void handle_output(const std::shared_ptr<gamepad_context_t> &gamepad, const lvh::GamepadOutput &output) {
      std::lock_guard lock {gamepad->feedback_mutex};
      switch (output.kind) {
        case lvh::GamepadOutputKind::rumble:
          if (gamepad->has_last_rumble && gamepad->last_low_frequency_rumble == output.low_frequency_rumble && gamepad->last_high_frequency_rumble == output.high_frequency_rumble) {
            return;
          }
          gamepad->has_last_rumble = true;
          gamepad->last_low_frequency_rumble = output.low_frequency_rumble;
          gamepad->last_high_frequency_rumble = output.high_frequency_rumble;
          raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_rumble(gamepad->client_relative_index, output.low_frequency_rumble, output.high_frequency_rumble));
          break;
        case lvh::GamepadOutputKind::trigger_rumble:
          if (gamepad->has_last_trigger_rumble && gamepad->last_left_trigger_rumble == output.left_trigger_rumble && gamepad->last_right_trigger_rumble == output.right_trigger_rumble) {
            return;
          }
          gamepad->has_last_trigger_rumble = true;
          gamepad->last_left_trigger_rumble = output.left_trigger_rumble;
          gamepad->last_right_trigger_rumble = output.right_trigger_rumble;
          raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_rumble_triggers(gamepad->client_relative_index, output.left_trigger_rumble, output.right_trigger_rumble));
          break;
        case lvh::GamepadOutputKind::rgb_led:
          if (gamepad->has_last_rgb && gamepad->last_red == output.red && gamepad->last_green == output.green && gamepad->last_blue == output.blue) {
            return;
          }
          gamepad->has_last_rgb = true;
          gamepad->last_red = output.red;
          gamepad->last_green = output.green;
          gamepad->last_blue = output.blue;
          raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_rgb_led(gamepad->client_relative_index, output.red, output.green, output.blue));
          break;
        case lvh::GamepadOutputKind::player_leds:
          {
            const auto solid = player_led_mask(output.player_leds);
            const auto flashing = player_led_mask(output.flashing_player_leds);
            if (gamepad->has_last_player_leds && gamepad->last_solid_player_leds == solid && gamepad->last_flashing_player_leds == flashing) {
              return;
            }
            gamepad->has_last_player_leds = true;
            gamepad->last_solid_player_leds = solid;
            gamepad->last_flashing_player_leds = flashing;
            raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_player_leds(gamepad->client_relative_index, solid, flashing));
            break;
          }
        case lvh::GamepadOutputKind::adaptive_triggers:
          raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_adaptive_triggers(gamepad->client_relative_index, output.adaptive_trigger_flags, output.left_trigger_effect_type, output.right_trigger_effect_type, output.left_trigger_effect, output.right_trigger_effect));
          break;
        case lvh::GamepadOutputKind::raw_report:
          break;
      }
    }

    void cancel_all_touches(client_context_t &context) {
      if (!context.touch) {
        return;
      }

      for (const auto id : context.active_touches) {
        log_failure("cancel libvirtualhid touch contact"sv, context.touch->cancel_contact(id));
      }
      context.active_touches.clear();
    }

  }  // namespace

  input_context_t::input_context_t():
      input_context_t {lvh::BackendKind::platform_default} {}

  input_context_t::input_context_t(lvh::BackendKind backend):
      runtime {create_runtime(backend)} {
    if (!runtime) {
      BOOST_LOG(warning) << "Unable to create libvirtualhid runtime"sv;
      return;
    }

    refresh_keyboard();
    refresh_mouse();
  }

  void input_context_t::refresh_keyboard() {
    keyboard.reset();
    if (!runtime || !runtime->capabilities().supports_keyboard) {
      return;
    }

    lvh::CreateKeyboardOptions options;
    options.profile = lvh::profiles::keyboard();
    apply_target_seat(options.profile);
    options.stable_id = "sunshine-keyboard";
    auto created = runtime->create_keyboard(options);
    if (created) {
      keyboard = std::move(created.keyboard);
    } else {
      log_failure("create libvirtualhid keyboard"sv, created.status);
    }
  }

  void input_context_t::refresh_mouse() {
    refresh_mouse({}, {});
  }

  void input_context_t::refresh_mouse(const lvh::PointerViewport &desktop, const lvh::PointerViewport &viewport) {
    mouse.reset();
    mouse_desktop = desktop;
    mouse_viewport = viewport;
    if (!runtime || !runtime->capabilities().supports_mouse) {
      return;
    }

    lvh::CreateMouseOptions options;
    options.profile = lvh::profiles::mouse();
    apply_target_seat(options.profile);
    options.stable_id = "sunshine-mouse";
    options.desktop = desktop;
    options.viewport = viewport;
    auto created = runtime->create_mouse(options);
    if (created) {
      mouse = std::move(created.mouse);
    } else {
      log_failure("create libvirtualhid mouse"sv, created.status);
    }
  }

  void input_context_t::update_mouse_viewport(const touch_port_t &touch_port) {
    const auto [desktop, viewport] = pointer_viewports(touch_port);
    if (mouse && pointer_viewports_equal(mouse_desktop, desktop) && pointer_viewports_equal(mouse_viewport, viewport)) {
      return;
    }

    refresh_mouse(desktop, viewport);
  }

  client_context_t::client_context_t(input_context_t &input):
      global {&input} {
    if (!global->runtime) {
      return;
    }

    const auto &capabilities = global->runtime->capabilities();
    if (capabilities.supports_touchscreen) {
      lvh::CreateTouchscreenOptions options;
      options.profile = lvh::profiles::touchscreen();
      apply_target_seat(options.profile);
      options.stable_id = "sunshine-touchscreen";
      auto created = global->runtime->create_touchscreen(options);
      if (created) {
        touch = std::move(created.touchscreen);
      } else {
        log_failure("create libvirtualhid touchscreen"sv, created.status);
      }
    }
    if (capabilities.supports_pen_tablet) {
      lvh::CreatePenTabletOptions options;
      options.profile = lvh::profiles::pen_tablet();
      apply_target_seat(options.profile);
      options.stable_id = "sunshine-pen-tablet";
      auto created = global->runtime->create_pen_tablet(options);
      if (created) {
        pen = std::move(created.pen_tablet);
      } else {
        log_failure("create libvirtualhid pen tablet"sv, created.status);
      }
    }
  }

  std::unique_ptr<lvh::Runtime> create_runtime(lvh::BackendKind backend) {
    lvh::RuntimeOptions options;
    options.backend = backend;
    options.log_callback = log_virtualhid_message;
    return lvh::Runtime::create(options);
  }

  std::vector<supported_gamepad_t> static_supported_gamepads() {
    std::vector gamepads {
      supported_gamepad_t {"auto", true, ""},
    };
    for (const auto &profile : gamepad_profiles) {
      gamepads.emplace_back(std::string {profile.name}, false, "");
    }

    return gamepads;
  }

  std::vector<supported_gamepad_t> supported_gamepads(
    lvh::Runtime *runtime,
    const bool fallback_vigem_available,
    const bool virtualhid_licensed,
    const bool require_license
  ) {
    if (!runtime) {
      return static_supported_gamepads();
    }

    const auto &capabilities = runtime->capabilities();
    const auto license_valid = (!capabilities.requires_installed_driver && !require_license) || virtualhid_licensed;
    const auto libvirtualhid_available = capabilities.supports_gamepad && license_valid;
    std::string reason;
    if (!capabilities.supports_gamepad) {
      reason = "gamepads.virtualhid-not-available";
    } else if (!license_valid) {
      reason = "gamepads.virtualhid-license-invalid";
    }
    const auto auto_enabled = libvirtualhid_available || fallback_vigem_available;
    std::vector gamepads {
      supported_gamepad_t {"auto", auto_enabled, auto_enabled ? "" : reason},
    };

    for (const auto &profile : gamepad_profiles) {
      const auto fallback_supported = fallback_vigem_available && (profile.name == "x360"sv || profile.name == "ds4"sv);
      const auto enabled = libvirtualhid_available || fallback_supported;
      gamepads.emplace_back(std::string {profile.name}, enabled, enabled ? "" : reason);
    }

    for (auto &[name, is_enabled, reason_disabled] : gamepads) {
      if (!is_enabled) {
        BOOST_LOG(warning) << "Gamepad "sv << name << " is disabled due to "sv << reason_disabled;
      }
    }

    return gamepads;
  }

  bool should_use_gamepad_runtime(
    const lvh::BackendCapabilities &capabilities,
    const std::string_view gamepad_driver,
    const bool virtualhid_licensed
  ) {
    return gamepad_driver != config::GAMEPAD_DRIVER_VIGEMBUS && gamepad_driver != config::GAMEPAD_DRIVER_NONE && capabilities.supports_gamepad &&
           (!capabilities.requires_installed_driver || virtualhid_licensed);
  }

  bool should_try_vigembus_fallback(
    const std::string_view configured_gamepad,
    const bool virtualhid_selected,
    const std::string_view gamepad_driver
  ) {
    if (gamepad_driver == config::GAMEPAD_DRIVER_VIRTUALHID || gamepad_driver == config::GAMEPAD_DRIVER_NONE) {
      return false;
    }
    if (gamepad_driver == config::GAMEPAD_DRIVER_VIGEMBUS || !virtualhid_selected) {
      return true;
    }
    return configured_gamepad == "auto"sv ||
           configured_gamepad == "x360"sv ||
           configured_gamepad == "ds4"sv;
  }

  int alloc_gamepad(input_context_t &context, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    if (!context.runtime || !context.runtime->capabilities().supports_gamepad) {
      return -1;
    }
    if (id.globalIndex < 0 || id.globalIndex >= static_cast<int>(context.gamepads.size())) {
      BOOST_LOG(warning) << "Invalid libvirtualhid gamepad index: "sv << id.globalIndex;
      return -1;
    }

    const auto &selection = profile_for_metadata(metadata);
    auto profile = selection.profile();
    profile.name = std::format("Sunshine {}", profile.name);
    apply_target_seat(profile);
    if (config::input.gamepad != "auto"sv) {
      BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be "sv << profile.name << " (manual selection)"sv;
    } else {
      BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be "sv << profile.name;
    }

    lvh::CreateGamepadOptions options;
    options.profile = profile;
    options.metadata = gamepad_metadata(id, metadata, profile);
    auto created = lvh::GamepadStateAdapter::create(*context.runtime, options);
    if (!created) {
      log_failure("create libvirtualhid gamepad"sv, created.status);
      return -1;
    }

    auto gamepad = std::make_shared<gamepad_context_t>();
    gamepad->adapter = std::move(created.adapter);
    gamepad->feedback_queue = std::move(feedback_queue);
    gamepad->client_relative_index = id.clientRelativeIndex;
    gamepad->adapter->set_output_callback([weak_gamepad = std::weak_ptr {gamepad}](const lvh::GamepadOutput &output) {
      if (const auto gamepad = weak_gamepad.lock()) {
        handle_output(gamepad, output);
      }
    });

    const auto &support = gamepad->adapter->support();
    warn_unsupported_client_features(id.globalIndex, metadata, support);
    warn_missing_client_features(id.globalIndex, metadata, support);
    if (support.supports_motion) {
      raise_feedback(gamepad, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
      raise_feedback(gamepad, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
    }

    context.gamepads[id.globalIndex] = std::move(gamepad);
    return 0;
  }

  int rebind_gamepad(input_context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    if (!has_gamepad(context, id.globalIndex)) {
      return -1;
    }

    const auto &gamepad = context.gamepads[id.globalIndex];
    std::lock_guard lock {gamepad->feedback_mutex};
    gamepad->feedback_queue = std::move(feedback_queue);
    gamepad->client_relative_index = id.clientRelativeIndex;
    gamepad->has_last_rumble = false;
    gamepad->has_last_trigger_rumble = false;
    gamepad->has_last_rgb = false;
    gamepad->has_last_player_leds = false;

    if (gamepad->adapter->support().supports_motion) {
      raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
      raise_feedback_unlocked(gamepad, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
    }

    return 0;
  }

  bool has_gamepad(const input_context_t &context, int nr) {
    return nr >= 0 && nr < context.gamepads.size() && context.gamepads[nr] && context.gamepads[nr]->adapter;
  }

#ifdef SUNSHINE_TESTS
  lvh::GamepadStateAdapter *gamepad_adapter_for_testing(input_context_t &context, int nr) {
    return has_gamepad(context, nr) ? context.gamepads[nr]->adapter.get() : nullptr;
  }
#endif

  void free_gamepad(input_context_t &context, int nr) {
    if (has_gamepad(context, nr)) {
      context.gamepads[nr].reset();
    }
  }

  void gamepad_update(input_context_t &context, int nr, const gamepad_state_t &state) {
    if (!has_gamepad(context, nr)) {
      return;
    }

    auto &gamepad = context.gamepads[nr];
    auto updated_state = make_gamepad_state(state, gamepad->adapter->support());
    const auto &cached_state = gamepad->adapter->state();
    updated_state.acceleration = cached_state.acceleration;
    updated_state.gyroscope = cached_state.gyroscope;
    updated_state.battery = cached_state.battery;
    updated_state.touchpad_contacts = cached_state.touchpad_contacts;
    log_failure("submit libvirtualhid gamepad state"sv, gamepad->adapter->set_state(updated_state));
  }

  void gamepad_touch(input_context_t &context, const gamepad_touch_t &touch) {
    if (!has_gamepad(context, touch.id.globalIndex)) {
      return;
    }

    auto &gamepad = context.gamepads[touch.id.globalIndex];
    if (!gamepad->adapter->support().supports_touchpad) {
      return;
    }

    if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      for (std::size_t index = 0; index < gamepad->touch_ids.size(); ++index) {
        if (gamepad->touch_ids[index].has_value()) {
          log_failure("release libvirtualhid gamepad touch"sv, gamepad->adapter->clear_touchpad_contact(index));
          gamepad->touch_ids[index].reset();
        }
      }
      return;
    }

    auto slot = std::ranges::find(gamepad->touch_ids, touch.pointerId);
    if (touch.eventType == LI_TOUCH_EVENT_DOWN && slot == gamepad->touch_ids.end()) {
      slot = std::ranges::find_if(gamepad->touch_ids, [](const auto &id) {
        return !id.has_value();
      });
      if (slot == gamepad->touch_ids.end()) {
        BOOST_LOG(warning) << "No free libvirtualhid gamepad touch slots"sv;
        return;
      }
      *slot = touch.pointerId;
    }

    if (slot == gamepad->touch_ids.end()) {
      return;
    }

    const auto index = static_cast<std::size_t>(std::distance(gamepad->touch_ids.begin(), slot));
    if (touch.eventType == LI_TOUCH_EVENT_UP || touch.eventType == LI_TOUCH_EVENT_CANCEL) {
      log_failure("release libvirtualhid gamepad touch"sv, gamepad->adapter->clear_touchpad_contact(index));
      slot->reset();
      return;
    }
    if (touch.eventType != LI_TOUCH_EVENT_DOWN && touch.eventType != LI_TOUCH_EVENT_MOVE) {
      return;
    }

    lvh::GamepadTouchContact contact;
    contact.id = static_cast<std::uint8_t>(index);
    contact.active = touch.pressure > 0.5F;
    contact.x = std::clamp(touch.x, 0.0F, 1.0F);
    contact.y = std::clamp(touch.y, 0.0F, 1.0F);
    log_failure("submit libvirtualhid gamepad touch"sv, gamepad->adapter->set_touchpad_contact(index, contact));
  }

  void gamepad_motion(input_context_t &context, const gamepad_motion_t &motion) {
    if (!has_gamepad(context, motion.id.globalIndex)) {
      return;
    }

    auto &gamepad = context.gamepads[motion.id.globalIndex];
    switch (motion.motionType) {
      case LI_MOTION_TYPE_ACCEL:
        log_failure("submit libvirtualhid gamepad acceleration"sv, gamepad->adapter->set_acceleration(lvh::Vector3 {motion.x, motion.y, motion.z}));
        break;
      case LI_MOTION_TYPE_GYRO:
        log_failure("submit libvirtualhid gamepad gyroscope"sv, gamepad->adapter->set_gyroscope(lvh::Vector3 {motion.x, motion.y, motion.z}));
        break;
      default:
        break;
    }
  }

  void gamepad_battery(input_context_t &context, const gamepad_battery_t &battery) {
    if (!has_gamepad(context, battery.id.globalIndex)) {
      return;
    }

    auto &gamepad = context.gamepads[battery.id.globalIndex];
    if (battery.state == LI_BATTERY_STATE_UNKNOWN || battery.state == LI_BATTERY_STATE_NOT_PRESENT) {
      log_failure("clear libvirtualhid gamepad battery"sv, gamepad->adapter->clear_battery());
      return;
    }

    lvh::GamepadBattery value;
    value.state = battery_state(battery.state);
    value.percentage = battery.percentage == LI_BATTERY_PERCENTAGE_UNKNOWN ? 100 : std::min<std::uint8_t>(battery.percentage, 100);
    log_failure("submit libvirtualhid gamepad battery"sv, gamepad->adapter->set_battery(value));
  }

  void move_mouse(input_context_t &context, int delta_x, int delta_y) {
    if (context.mouse) {
      log_failure("submit libvirtualhid mouse movement"sv, context.mouse->move_relative(delta_x, delta_y));
    }
  }

  void move_mouse(input_context_t &context, const touch_port_t &touch_port, int delta_x, int delta_y) {
    if (!context.mouse) {
      return;
    }

    context.update_mouse_viewport(touch_port);
    move_mouse(context, delta_x, delta_y);
  }

  void abs_mouse(input_context_t &context, const touch_port_t &touch_port, float x, float y) {
    if (!context.mouse) {
      return;
    }

    context.update_mouse_viewport(touch_port);
    if (!context.mouse) {
      return;
    }

    log_failure(
      "submit libvirtualhid absolute mouse movement"sv,
      context.mouse->move_absolute(
        x - static_cast<float>(context.mouse_viewport.offset_x),
        y - static_cast<float>(context.mouse_viewport.offset_y),
        context.mouse_viewport.width,
        context.mouse_viewport.height
      )
    );
  }

  void button_mouse(input_context_t &context, int button, bool release) {
    if (context.mouse) {
      const auto converted = mouse_button(button);
      if (!converted) {
        return;
      }

      log_failure("submit libvirtualhid mouse button"sv, context.mouse->button(*converted, !release));
    }
  }

  void scroll(input_context_t &context, int high_res_distance) {
    if (context.mouse) {
      log_failure("submit libvirtualhid vertical scroll"sv, context.mouse->vertical_scroll(high_res_distance));
    }
  }

  void hscroll(input_context_t &context, int high_res_distance) {
    if (context.mouse) {
      log_failure("submit libvirtualhid horizontal scroll"sv, context.mouse->horizontal_scroll(high_res_distance));
    }
  }

  void keyboard_update(input_context_t &context, std::uint16_t modcode, bool release, std::uint8_t flags, bool extended) {
    if (context.keyboard) {
      log_failure("submit libvirtualhid keyboard input"sv, context.keyboard->submit(keyboard_event(modcode, release, flags, extended)));
    }
  }

  void unicode(input_context_t &context, const char *utf8, int size) {
    if (context.keyboard && utf8 && size > 0) {
      log_failure("submit libvirtualhid text input"sv, context.keyboard->type_text({.text = std::string {utf8, static_cast<std::size_t>(size)}}));
    }
  }

  void touch_update(client_context_t &context, const touch_port_t &touch_port, const touch_input_t &touch) {
    if (!context.touch) {
      return;
    }

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_CANCEL_ALL:
        cancel_all_touches(context);
        return;
      case LI_TOUCH_EVENT_UP:
        log_failure("release libvirtualhid touch contact"sv, context.touch->release_contact(static_cast<std::int32_t>(touch.pointerId)));
        context.active_touches.erase(static_cast<std::int32_t>(touch.pointerId));
        return;
      case LI_TOUCH_EVENT_CANCEL:
        log_failure("cancel libvirtualhid touch contact"sv, context.touch->cancel_contact(static_cast<std::int32_t>(touch.pointerId)));
        context.active_touches.erase(static_cast<std::int32_t>(touch.pointerId));
        return;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        log_failure("leave libvirtualhid touch contact"sv, context.touch->leave_contact(static_cast<std::int32_t>(touch.pointerId)));
        context.active_touches.erase(static_cast<std::int32_t>(touch.pointerId));
        return;
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        {
          lvh::TouchContact contact;
          contact.id = static_cast<std::int32_t>(touch.pointerId);
          contact.x = std::clamp(touch.x, 0.0F, 1.0F);
          contact.y = std::clamp(touch.y, 0.0F, 1.0F);
          contact.pressure = std::clamp(touch.pressureOrDistance, 0.0F, 1.0F);
          contact.orientation = touch_orientation(touch.rotation);
          contact.touching = touch.eventType != LI_TOUCH_EVENT_HOVER;
          contact.viewport = pointer_viewport(touch_port);
          contact.contact_major_axis = touch.contactAreaMajor;
          contact.contact_minor_axis = touch.contactAreaMinor;
          log_failure("submit libvirtualhid touch contact"sv, context.touch->place_contact(contact));
          context.active_touches.insert(contact.id);
          return;
        }
      default:
        return;
    }
  }

  void pen_update(client_context_t &context, const touch_port_t &touch_port, const pen_input_t &pen) {
    if (!context.pen) {
      return;
    }

    const auto pen_buttons = static_cast<std::byte>(pen.penButtons);
    const std::array button_states {
      std::pair {lvh::PenButton::primary, (pen_buttons & static_cast<std::byte>(LI_PEN_BUTTON_PRIMARY)) != std::byte {}},
      std::pair {lvh::PenButton::secondary, (pen_buttons & static_cast<std::byte>(LI_PEN_BUTTON_SECONDARY)) != std::byte {}},
      std::pair {lvh::PenButton::tertiary, (pen_buttons & static_cast<std::byte>(LI_PEN_BUTTON_TERTIARY)) != std::byte {}},
    };
    for (const auto &[button, pressed] : button_states) {
      const auto was_pressed = context.pressed_pen_buttons.contains(button);
      if (pressed == was_pressed) {
        continue;
      }

      log_failure("submit libvirtualhid pen button"sv, context.pen->button(button, pressed));
      if (pressed) {
        context.pressed_pen_buttons.insert(button);
      } else {
        context.pressed_pen_buttons.erase(button);
      }
    }

    if (pen.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      for (const auto button : context.pressed_pen_buttons) {
        log_failure("release libvirtualhid pen button"sv, context.pen->button(button, false));
      }
      context.pressed_pen_buttons.clear();
    }

    using enum lvh::PointerTransition;
    auto transition = update;
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        transition = cancel;
        break;
      case LI_TOUCH_EVENT_UP:
        transition = release;
        break;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        transition = leave;
        break;
      default:
        break;
    }

    auto rotation = pen.rotation;
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    float tilt_x = 0.0F;
    float tilt_y = 0.0F;
    if (pen.tilt != LI_TILT_UNKNOWN && rotation != LI_ROT_UNKNOWN) {
      const auto rotation_rads = static_cast<float>(rotation) * std::numbers::pi_v<float> / 180.0F;
      const auto tilt_rads = static_cast<float>(pen.tilt) * std::numbers::pi_v<float> / 180.0F;
      const auto r = std::sin(tilt_rads);
      const auto z = std::cos(tilt_rads);

      tilt_x = std::atan2(std::sin(-rotation_rads) * r, z) * 180.0F / std::numbers::pi_v<float>;
      tilt_y = std::atan2(std::cos(-rotation_rads) * r, z) * 180.0F / std::numbers::pi_v<float>;
    }

    const auto is_touching = transition == update &&
                             (pen.eventType == LI_TOUCH_EVENT_DOWN || pen.eventType == LI_TOUCH_EVENT_MOVE);
    lvh::PenToolState state;
    state.tool = pen_tool(pen.toolType);
    state.x = std::clamp(pen.x, 0.0F, 1.0F);
    state.y = std::clamp(pen.y, 0.0F, 1.0F);
    state.pressure = is_touching ? std::clamp(pen.pressureOrDistance, 0.0F, 1.0F) : -1.0F;
    state.distance = is_touching ? -1.0F : std::clamp(pen.pressureOrDistance, 0.0F, 1.0F);
    state.tilt_x = tilt_x;
    state.tilt_y = tilt_y;
    state.transition = transition;
    state.viewport = pointer_viewport(touch_port);
    log_failure("submit libvirtualhid pen state"sv, context.pen->place_tool(state));
  }

  bool configured_gamepad_supports_touchpad() {
    if (config::input.gamepad == "auto"sv) {
      return true;
    }

    const auto profile = profile_for_name(config::input.gamepad).profile();
    return lvh::gamepad_profile_support(profile).supports_touchpad;
  }

  bool configured_gamepad_supports_controller_extensions() {
    if (config::input.gamepad == "auto"sv) {
      return true;
    }

    const auto profile = profile_for_name(config::input.gamepad).profile();
    const auto &support = lvh::gamepad_profile_support(profile);
    return support.supports_touchpad || support.supports_motion;
  }

  std::string target_seat() {
    return lizardbyte::common::get_env("XDG_SEAT");
  }

  std::string device_name_for_seat(std::string_view base_name, std::string_view seat) {
    if (seat.empty() || seat == "seat0"sv) {
      return std::string {base_name};
    }

    return std::format("{} ({})", base_name, seat);
  }

}  // namespace platf::virtualhid

namespace platf {

#ifndef _WIN32
  /**
   * @brief Global libvirtualhid devices shared by clients.
   */
  struct input_raw_t {
    virtualhid::input_context_t virtualhid;  ///< libvirtualhid input context.
  };

  namespace {

    /**
     * @brief Per-client libvirtualhid devices.
     */
    struct client_input_raw_t: client_input_t {
      /**
       * @brief Create per-client libvirtualhid devices.
       *
       * @param input Platform input backend that receives the event.
       */
      explicit client_input_raw_t(input_t &input):
          virtualhid {input->virtualhid} {}

      virtualhid::client_context_t virtualhid;  ///< libvirtualhid client context.
    };

  }  // namespace

  /**
   * @brief Create the libvirtualhid backend with explicit input-handle ownership.
   *
   * @return Owning handle for the global virtual input backend.
   */
  input_t input() {
    return input_t {new input_raw_t {}};
  }

  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  void freeInput(input_raw_t *input) {
    std::default_delete<input_raw_t> {}(input);
  }

  virtualhid::input_context_t &virtualhid::get_input_context(input_t &input) {
    return input->virtualhid;
  }

  virtualhid::client_context_t &virtualhid::get_client_context(client_input_t *input) {
    return static_cast<client_input_raw_t *>(input)->virtualhid;
  }
#endif

  void move_mouse(input_t &input, int deltaX, int deltaY) {
    virtualhid::move_mouse(virtualhid::get_input_context(input), deltaX, deltaY);
  }

  void move_mouse(input_t &input, const touch_port_t &touch_port, int deltaX, int deltaY) {
    virtualhid::move_mouse(virtualhid::get_input_context(input), touch_port, deltaX, deltaY);
  }

  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    virtualhid::abs_mouse(virtualhid::get_input_context(input), touch_port, x, y);
  }

  void button_mouse(input_t &input, int button, bool release) {
    virtualhid::button_mouse(virtualhid::get_input_context(input), button, release);
  }

  void scroll(input_t &input, int high_res_distance) {
    virtualhid::scroll(virtualhid::get_input_context(input), high_res_distance);
  }

  void hscroll(input_t &input, int high_res_distance) {
    virtualhid::hscroll(virtualhid::get_input_context(input), high_res_distance);
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags, bool extended) {
    virtualhid::keyboard_update(virtualhid::get_input_context(input), modcode, release, flags, extended);
  }

  void unicode(input_t &input, const char *utf8, int size) {
    virtualhid::unicode(virtualhid::get_input_context(input), utf8, size);
  }

  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    virtualhid::touch_update(virtualhid::get_client_context(input), touch_port, touch);
  }

  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    virtualhid::pen_update(virtualhid::get_client_context(input), touch_port, pen);
  }

#ifndef _WIN32
  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    return virtualhid::alloc_gamepad(virtualhid::get_input_context(input), id, metadata, std::move(feedback_queue));
  }

  int rebind_gamepad(input_t &input, const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    return virtualhid::rebind_gamepad(virtualhid::get_input_context(input), id, std::move(feedback_queue));
  }

  void free_gamepad(input_t &input, int nr) {
    virtualhid::free_gamepad(virtualhid::get_input_context(input), nr);
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    virtualhid::gamepad_update(virtualhid::get_input_context(input), nr, gamepad_state);
  }

  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    virtualhid::gamepad_touch(virtualhid::get_input_context(input), touch);
  }

  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    virtualhid::gamepad_motion(virtualhid::get_input_context(input), motion);
  }

  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    virtualhid::gamepad_battery(virtualhid::get_input_context(input), battery);
  }
#endif

}  // namespace platf
