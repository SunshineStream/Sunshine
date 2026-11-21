// SPDX-License-Identifier: GPL-3.0-only
#include <winsock2.h>
#include <windows.h>

#include "open_vhf_input.h"

#include <libvirtualgamepad/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace platf {
  using namespace std::chrono_literals;

  namespace {
    using lvg::profile;
    constexpr std::array profiles {
      std::pair {"xone"sv, profile::xbox_one},
      std::pair {"xseries"sv, profile::xbox_series},
      std::pair {"ds4"sv, profile::dualshock_4},
      std::pair {"ds5"sv, profile::dualsense},
      std::pair {"switch"sv, profile::switch_pro},
    };

    static_assert(MAX_GAMEPADS <= lvg::k_max_controllers);
    static_assert(A == lvg::south && B == lvg::east && X == lvg::west && Y == lvg::north);
    static_assert(DPAD_UP == lvg::dpad_up && DPAD_DOWN == lvg::dpad_down && DPAD_LEFT == lvg::dpad_left && DPAD_RIGHT == lvg::dpad_right);
    static_assert(START == lvg::start && BACK == lvg::back && HOME == lvg::home);
    static_assert(LEFT_STICK == lvg::left_stick && RIGHT_STICK == lvg::right_stick);
    static_assert(LEFT_BUTTON == lvg::left_shoulder && RIGHT_BUTTON == lvg::right_shoulder);
    static_assert(PADDLE1 == lvg::paddle_1 && PADDLE2 == lvg::paddle_2 && PADDLE3 == lvg::paddle_3 && PADDLE4 == lvg::paddle_4);
    static_assert(TOUCHPAD_BUTTON == lvg::touchpad && MISC_BUTTON == lvg::misc);

    bool playstation(profile model) {
      return model == profile::dualshock_4 || model == profile::dualsense;
    }

    bool has_motion(profile model) {
      return playstation(model) || model == profile::switch_pro;
    }

    std::optional<profile> choose_profile(lvg::profile_mask_t available, std::string_view name, const gamepad_arrival_t &arrival = {}) {
      auto offers = [available](profile model) { return (available & lvg::profile_bit(model)) != 0; };
      if (name != "auto") {
        for (const auto &[label, model] : profiles) {
          if (name == label && offers(model)) {
            return model;
          }
        }
        return std::nullopt;
      }

      if (arrival.type == LI_CTYPE_NINTENDO && offers(profile::switch_pro)) {
        return profile::switch_pro;
      }
      if (arrival.type == LI_CTYPE_PS ||
          (arrival.type == LI_CTYPE_UNKNOWN && ((config::input.motion_as_ds4 && (arrival.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) ||
                                                (config::input.touchpad_as_ds4 && (arrival.capabilities & LI_CCAP_TOUCHPAD))))) {
        for (const auto model : {profile::dualsense, profile::dualshock_4}) {
          if (offers(model)) {
            return model;
          }
        }
      }
      for (const auto &[label, model] : profiles) {
        if (offers(model)) {
          return model;
        }
      }
      return std::nullopt;
    }

    template<class Request>
    Request request_for(int index) {
      Request request {};
      request.header = {sizeof(Request), lvg::k_protocol_version, 0};
      request.controller_id = static_cast<std::uint32_t>(index);
      return request;
    }

    template<class Payload>
    std::optional<Payload> payload_of(const lvg::feedback_event &event) {
      if (event.payload_size != sizeof(Payload)) {
        return std::nullopt;
      }
      Payload payload {};
      std::memcpy(&payload, event.payload, sizeof(payload));
      return payload;
    }

    struct slot_t {
      std::optional<profile> model;
      feedback_queue_t queue;
      std::uint8_t client_index = 0;
      std::uint16_t capabilities = 0;
      std::array<std::optional<std::uint32_t>, 2> contacts {};

      void request_motion() const {
        if (queue && model && has_motion(*model)) {
          if (capabilities & LI_CCAP_ACCEL) {
            queue->raise(gamepad_feedback_msg_t::make_motion_event_state(client_index, LI_MOTION_TYPE_ACCEL, 100));
          }
          if (capabilities & LI_CCAP_GYRO) {
            queue->raise(gamepad_feedback_msg_t::make_motion_event_state(client_index, LI_MOTION_TYPE_GYRO, 100));
          }
        }
      }

      void feedback(const lvg::feedback_event &event) const {
        if (!queue) {
          return;
        }
        const auto id = client_index;
        if (event.type == lvg::feedback_type::xbox_rumble) {
          if (auto output = payload_of<lvg::xbox_rumble_feedback>(event)) {
            queue->raise(gamepad_feedback_msg_t::make_rumble(id, output->low_frequency, output->high_frequency));
            queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(id, output->left_trigger, output->right_trigger));
          }
        } else if (event.type == lvg::feedback_type::playstation_output) {
          if (auto output = payload_of<lvg::playstation_output_feedback>(event)) {
            queue->raise(gamepad_feedback_msg_t::make_rumble(id, output->low_frequency, output->high_frequency));
            if (output->valid & lvg::ps_output_lightbar_valid) {
              queue->raise(gamepad_feedback_msg_t::make_rgb_led(id, output->red, output->green, output->blue));
            }
            if (output->valid & lvg::ps_output_triggers_valid) {
              std::array<std::uint8_t, 10> left {}, right {};
              std::copy_n(output->left_trigger.parameters, left.size(), left.begin());
              std::copy_n(output->right_trigger.parameters, right.size(), right.begin());
              // Sunshine transports the DualSense output report's right and left enable bits.
              constexpr std::uint8_t both_triggers = 0x04 | 0x08;
              queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(id, both_triggers, output->left_trigger.mode, output->right_trigger.mode, left, right));
            }
          }
        } else if (event.type == lvg::feedback_type::generic_rumble || event.type == lvg::feedback_type::generic_rumble_rgb) {
          if (auto output = payload_of<lvg::generic_rumble_rgb_feedback>(event)) {
            queue->raise(gamepad_feedback_msg_t::make_rumble(id, output->low_frequency, output->high_frequency));
          }
        }
      }
    };

    bool valid_index(int index) {
      return index >= 0 && index < MAX_GAMEPADS;
    }
  }

  struct open_vhf_input_t::implementation_t {
    bool connect() {
      return client.connected() || client.connect() == ERROR_SUCCESS;
    }

    void feedback_loop(std::stop_token stop) {
      while (!stop.stop_requested()) {
        {
          std::lock_guard lock {mutex};
          for (int index = 0; index < MAX_GAMEPADS; ++index) {
            if (slots[index].model) {
              lvg::feedback_event event {};
              if (client.poll_feedback(index, &event) == ERROR_SUCCESS) {
                slots[index].feedback(event);
              }
            }
          }
        }
        std::this_thread::sleep_for(8ms);
      }
    }

    std::mutex mutex;
    lvg::client client;
    std::array<slot_t, MAX_GAMEPADS> slots {};
    std::jthread feedback_thread;
  };

  open_vhf_input_t::open_vhf_input_t(): implementation {std::make_unique<implementation_t>()} {}
  open_vhf_input_t::~open_vhf_input_t() = default;

  bool open_vhf_input_t::available(std::string_view name) {
    std::lock_guard lock {implementation->mutex};
    return implementation->connect() && choose_profile(implementation->client.available_profiles(), name).has_value();
  }

  bool open_vhf_input_t::allocate(const gamepad_id_t &id, const gamepad_arrival_t &arrival, const feedback_queue_t &queue) {
    const auto index = id.globalIndex;
    if (!valid_index(index)) {
      return false;
    }
    std::lock_guard lock {implementation->mutex};
    auto &slot = implementation->slots[index];
    if (slot.model || !implementation->connect()) {
      return false;
    }
    auto &client = implementation->client;
    const auto model = choose_profile(client.available_profiles(), config::input.gamepad, arrival);
    if (!model || client.create_controller(index, *model) != ERROR_SUCCESS) {
      return false;
    }
    slot = {*model, queue, id.clientRelativeIndex, arrival.capabilities, {}};
    if (!implementation->feedback_thread.joinable()) {
      implementation->feedback_thread = std::jthread([state = implementation.get()](std::stop_token stop) { state->feedback_loop(stop); });
    }
    slot.request_motion();
    BOOST_LOG(info) << "Open VHF virtual controller allocated at slot " << index;
    return true;
  }

  bool open_vhf_input_t::owns(int index) {
    std::lock_guard lock {implementation->mutex};
    return valid_index(index) && implementation->slots[index].model.has_value();
  }

  int open_vhf_input_t::rebind(const gamepad_id_t &id, feedback_queue_t queue) {
    std::lock_guard lock {implementation->mutex};
    auto &slot = implementation->slots[id.globalIndex];
    slot.queue = std::move(queue);
    slot.client_index = id.clientRelativeIndex;
    slot.request_motion();
    return 0;
  }

  void open_vhf_input_t::destroy(int index) {
    std::lock_guard lock {implementation->mutex};
    const auto result = implementation->client.destroy_controller(index);
    if (result != ERROR_SUCCESS && result != ERROR_NOT_FOUND && result != ERROR_DEVICE_REMOVED) {
      BOOST_LOG(warning) << "Open VHF could not release slot " << index << ": " << result;
    }
    implementation->slots[index] = {};
  }

  void open_vhf_input_t::update(int index, const gamepad_state_t &state) {
    auto request = request_for<lvg::input_state_request>(index);
    request.buttons = state.buttonFlags;
    request.left_x = state.lsX;
    request.left_y = state.lsY;
    request.right_x = state.rsX;
    request.right_y = state.rsY;
    request.left_trigger = state.lt;
    request.right_trigger = state.rt;
    std::lock_guard lock {implementation->mutex};
    auto &slot = implementation->slots[index];
    if (playstation(*slot.model) && config::input.ds4_back_as_touchpad_click && (request.buttons & BACK)) {
      request.buttons |= TOUCHPAD_BUTTON;
    }
    auto &client = implementation->client;
    if (client.submit_input_state(request) == ERROR_DEVICE_REMOVED) {
      if (client.create_controller(index, *slot.model) == ERROR_SUCCESS) {
        slot.contacts = {};
        slot.request_motion();
        static_cast<void>(client.submit_input_state(request));
      }
    }
  }

  void open_vhf_input_t::touch(const gamepad_touch_t &event) {
    const auto index = event.id.globalIndex;
    std::lock_guard lock {implementation->mutex};
    auto &slot = implementation->slots[index];
    if (!playstation(*slot.model)) {
      return;
    }

    lvg::touch_event type;
    switch (event.eventType) {
      case LI_TOUCH_EVENT_DOWN: type = lvg::touch_event::down; break;
      case LI_TOUCH_EVENT_UP: type = lvg::touch_event::up; break;
      case LI_TOUCH_EVENT_MOVE: type = lvg::touch_event::move; break;
      case LI_TOUCH_EVENT_CANCEL: type = lvg::touch_event::cancel; break;
      case LI_TOUCH_EVENT_CANCEL_ALL: type = lvg::touch_event::cancel_all; break;
      default: return;
    }
    auto request = request_for<lvg::touch_state_request>(index);
    request.event_type = static_cast<std::uint8_t>(type);
    auto &client = implementation->client;
    if (type == lvg::touch_event::cancel_all) {
      if (client.submit_touch_state(request) == ERROR_SUCCESS) {
        slot.contacts = {};
      }
      return;
    }
    auto it = std::find(slot.contacts.begin(), slot.contacts.end(), event.pointerId);
    if (it == slot.contacts.end() && type == lvg::touch_event::down) {
      it = std::find(slot.contacts.begin(), slot.contacts.end(), std::nullopt);
    }
    if (it == slot.contacts.end()) {
      return;
    }
    const auto normalized = [](float value) {
      return std::isfinite(value) ? static_cast<std::uint16_t>(std::clamp(value, 0.0f, 1.0f) * 65535.0f) : std::uint16_t {0};
    };
    request.contact_index = static_cast<std::uint8_t>(it - slot.contacts.begin());
    request.x = normalized(event.x);
    request.y = normalized(event.y);
    request.pressure = normalized(event.pressure);
    if (client.submit_touch_state(request) == ERROR_SUCCESS) {
      if (type == lvg::touch_event::down) {
        *it = event.pointerId;
      } else if (type == lvg::touch_event::up || type == lvg::touch_event::cancel) {
        it->reset();
      }
    }
  }

  void open_vhf_input_t::motion(const gamepad_motion_t &event) {
    const auto to_milli = [](float value) {
      const auto scaled = std::isfinite(value) ? static_cast<double>(value) * 1000.0 : 0.0;
      constexpr double minimum = (std::numeric_limits<std::int32_t>::min)();
      constexpr double maximum = (std::numeric_limits<std::int32_t>::max)();
      return static_cast<std::int32_t>(std::clamp(scaled, minimum, maximum));
    };
    const auto index = event.id.globalIndex;
    std::lock_guard lock {implementation->mutex};
    if (!has_motion(*implementation->slots[index].model)) {
      return;
    }
    auto request = request_for<lvg::motion_state_request>(index);
    request.motion_type = event.motionType;
    request.x_milli = to_milli(event.x);
    request.y_milli = to_milli(event.y);
    request.z_milli = to_milli(event.z);
    static_cast<void>(implementation->client.submit_motion_state(request));
  }

  void open_vhf_input_t::battery(const gamepad_battery_t &event) {
    const auto index = event.id.globalIndex;
    std::lock_guard lock {implementation->mutex};
    if (!has_motion(*implementation->slots[index].model)) {
      return;
    }
    auto request = request_for<lvg::battery_state_request>(index);
    auto state = lvg::battery_state::unknown;
    switch (event.state) {
      case LI_BATTERY_STATE_NOT_PRESENT: state = lvg::battery_state::not_present; break;
      case LI_BATTERY_STATE_DISCHARGING: state = lvg::battery_state::discharging; break;
      case LI_BATTERY_STATE_CHARGING: state = lvg::battery_state::charging; break;
      case LI_BATTERY_STATE_NOT_CHARGING: state = lvg::battery_state::not_charging; break;
      case LI_BATTERY_STATE_FULL: state = lvg::battery_state::full; break;
      default: break;
    }
    request.flags = static_cast<std::uint8_t>(state);
    request.percent = event.percentage;
    static_cast<void>(implementation->client.submit_battery_state(request));
  }
}
