/**
 * @file src/input/gamepad.cpp
 * @brief Definitions for common gamepad input.
 */
#include "src/input/gamepad.h"
#include "src/input/common.h"
#include "src/input/init.h"
#include "src/input/platform_input.h"
#include "src/input/processor.h"

#include "src/globals.h"
#include "src/thread_pool.h"

using namespace std::literals;

namespace input::gamepad {

  template <std::size_t N>
  int
  alloc_id(std::bitset<N> &gamepad_mask) {
    for (int x = 0; x < gamepad_mask.size(); ++x) {
      if (!gamepad_mask[x]) {
        gamepad_mask[x] = true;
        return x;
      }
    }

    return -1;
  }

  template <std::size_t N>
  void
  free_id(std::bitset<N> &gamepad_mask, int id) {
    gamepad_mask[id] = false;
  }

  void
  free_gamepad(platf::input_t &platf_input, int id) {
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});
    platf::free_gamepad(platf_input, id);

    free_id(gamepadMask, id);
  }

  void
  print(PNV_MULTI_CONTROLLER_PACKET packet) {
    // Moonlight spams controller packet even when not necessary
    BOOST_LOG(verbose)
      << "--begin controller packet--"sv << std::endl
      << "controllerNumber ["sv << packet->controllerNumber << ']' << std::endl
      << "activeGamepadMask ["sv << util::hex(packet->activeGamepadMask).to_string_view() << ']' << std::endl
      << "buttonFlags ["sv << util::hex((uint32_t) packet->buttonFlags | (packet->buttonFlags2 << 16)).to_string_view() << ']' << std::endl
      << "leftTrigger ["sv << util::hex(packet->leftTrigger).to_string_view() << ']' << std::endl
      << "rightTrigger ["sv << util::hex(packet->rightTrigger).to_string_view() << ']' << std::endl
      << "leftStickX ["sv << packet->leftStickX << ']' << std::endl
      << "leftStickY ["sv << packet->leftStickY << ']' << std::endl
      << "rightStickX ["sv << packet->rightStickX << ']' << std::endl
      << "rightStickY ["sv << packet->rightStickY << ']' << std::endl
      << "--end controller packet--"sv;
  }

  void
  print(PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller arrival packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "type ["sv << util::hex(packet->type).to_string_view() << ']' << std::endl
      << "capabilities ["sv << util::hex(packet->capabilities).to_string_view() << ']' << std::endl
      << "supportedButtonFlags ["sv << util::hex(packet->supportedButtonFlags).to_string_view() << ']' << std::endl
      << "--end controller arrival packet--"sv;
  }

  void
  print(PSS_CONTROLLER_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller touch packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressure ["sv << from_netfloat(packet->pressure) << ']' << std::endl
      << "--end controller touch packet--"sv;
  }

  void
  print(PSS_CONTROLLER_MOTION_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller motion packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "motionType ["sv << util::hex(packet->motionType).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "z ["sv << from_netfloat(packet->z) << ']' << std::endl
      << "--end controller motion packet--"sv;
  }

  void
  print(PSS_CONTROLLER_BATTERY_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller battery packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "batteryState ["sv << util::hex(packet->batteryState).to_string_view() << ']' << std::endl
      << "batteryPercentage ["sv << util::hex(packet->batteryPercentage).to_string_view() << ']' << std::endl
      << "--end controller battery packet--"sv;
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads->size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    if ((*input->gamepads)[packet->controllerNumber].id >= 0) {
      BOOST_LOG(warning) << "ControllerNumber already allocated ["sv << packet->controllerNumber << ']';
      return;
    }

    platf::gamepad_arrival_t arrival {
      packet->type,
      util::endian::little(packet->capabilities),
      util::endian::little(packet->supportedButtonFlags),
    };

    auto id = alloc_id(gamepadMask);
    if (id < 0) {
      return;
    }

    // Allocate a new gamepad
    if (platf::alloc_gamepad(PlatformInput::getInstance(), { id, packet->controllerNumber }, arrival, input->feedback_queue)) {
      free_id(gamepadMask, id);
      return;
    }

    (*input->gamepads)[packet->controllerNumber].id = id;
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_TOUCH_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads->size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = (*input->gamepads)[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_touch_t touch {
      { gamepad.id, packet->controllerNumber },
      packet->eventType,
      util::endian::little(packet->pointerId),
      from_clamped_netfloat(packet->x, 0.0f, 1.0f),
      from_clamped_netfloat(packet->y, 0.0f, 1.0f),
      from_clamped_netfloat(packet->pressure, 0.0f, 1.0f),
    };

    platf::gamepad_touch(PlatformInput::getInstance(), touch);
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_MOTION_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads->size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = (*input->gamepads)[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_motion_t motion {
      { gamepad.id, packet->controllerNumber },
      packet->motionType,
      from_netfloat(packet->x),
      from_netfloat(packet->y),
      from_netfloat(packet->z),
    };

    platf::gamepad_motion(PlatformInput::getInstance(), motion);
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads->size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';

      return;
    }

    auto &gamepad = (*input->gamepads)[packet->controllerNumber];

    // If this is an event for a new gamepad, create the gamepad now. Ideally, the client would
    // send a controller arrival instead of this but it's still supported for legacy clients.
    if ((packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id < 0) {
      auto id = alloc_id(gamepadMask);
      if (id < 0) {
        return;
      }

      if (platf::alloc_gamepad(PlatformInput::getInstance(), { id, (uint8_t) packet->controllerNumber }, {}, input->feedback_queue)) {
        free_id(gamepadMask, id);
        return;
      }

      gamepad.id = id;
    }
    else if (!(packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id >= 0) {
      // If this is the final event for a gamepad being removed, free the gamepad and return.
      free_gamepad(PlatformInput::getInstance(), gamepad.id);
      gamepad.id = -1;
      return;
    }

    // If this gamepad has not been initialized, ignore it.
    // This could happen when platf::alloc_gamepad fails
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    std::uint16_t bf = packet->buttonFlags;
    std::uint32_t bf2 = packet->buttonFlags2;
    platf::gamepad_state_t gamepad_state {
      bf | (bf2 << 16),
      packet->leftTrigger,
      packet->rightTrigger,
      packet->leftStickX,
      packet->leftStickY,
      packet->rightStickX,
      packet->rightStickY
    };

    auto bf_new = gamepad_state.buttonFlags;
    switch (gamepad.back_button_state) {
      case button_state_e::UP:
        if (!(platf::BACK & bf_new)) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags &= ~platf::BACK;
        break;
      case button_state_e::DOWN:
        if (platf::BACK & bf_new) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags |= platf::BACK;
        break;
      case button_state_e::NONE:
        break;
    }

    bf = gamepad_state.buttonFlags ^ gamepad.gamepad_state.buttonFlags;
    bf_new = gamepad_state.buttonFlags;

    if (platf::BACK & bf) {
      if (platf::BACK & bf_new) {
        // Don't emulate home button if timeout < 0
        if (config::input.back_button_timeout >= 0ms) {
          auto f = [input, controller = packet->controllerNumber]() {
            auto &gamepad = (*input->gamepads)[controller];

            auto &state = gamepad.gamepad_state;

            // Force the back button up
            gamepad.back_button_state = button_state_e::UP;
            state.buttonFlags &= ~platf::BACK;
            platf::gamepad_update(PlatformInput::getInstance(), gamepad.id, state);

            // Press Home button
            state.buttonFlags |= platf::HOME;
            platf::gamepad_update(PlatformInput::getInstance(), gamepad.id, state);

            // Sleep for a short time to allow the input to be detected
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Release Home button
            state.buttonFlags &= ~platf::HOME;
            platf::gamepad_update(PlatformInput::getInstance(), gamepad.id, state);

            gamepad.back_timeout_id = nullptr;
          };

          gamepad.back_timeout_id = task_pool.pushDelayed(std::move(f), config::input.back_button_timeout).task_id;
        }
      }
      else if (gamepad.back_timeout_id) {
        task_pool.cancel(gamepad.back_timeout_id);
        gamepad.back_timeout_id = nullptr;
      }
    }

    platf::gamepad_update(PlatformInput::getInstance(), gamepad.id, gamepad_state);

    gamepad.gamepad_state = gamepad_state;
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_BATTERY_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads->size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = (*input->gamepads)[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_battery_t battery {
      { gamepad.id, packet->controllerNumber },
      packet->batteryState,
      packet->batteryPercentage
    };

    platf::gamepad_battery(PlatformInput::getInstance(), battery);
  }

  batch_result_e
  batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE &&
        src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  batch_result_e
  batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src) {
    // Do not allow batching if the active controllers change
    if (dest->activeGamepadMask != src->activeGamepadMask) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Do not allow batching if the button state changes on this controller
    if (dest->buttonFlags != src->buttonFlags || dest->buttonFlags2 != src->buttonFlags2) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  batch_result_e
  batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src) {
    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Batched events must be the same sensor
    if (dest->motionType != src->motionType) {
      return batch_result_e::not_batchable;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

}  // namespace input::gamepad
