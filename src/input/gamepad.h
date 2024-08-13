/**
 * @file src/input/gamepad.h
 * @brief Declarations for common gamepad input.
 */
#pragma once

#include "platform_input.h"

#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <functional>

#include "src/globals.h"
#include "src/platform/common.h"
#include "src/thread_pool.h"

#include "src/input/init.h"

namespace input::gamepad {
  static std::bitset<platf::MAX_GAMEPADS> gamepadMask = {};

  constexpr auto MAX_GAMEPADS = std::min(static_cast<std::size_t>(platf::MAX_GAMEPADS), sizeof(std::int16_t) * 8);

  enum class button_state_e {
    NONE,  ///< No button state
    DOWN,  ///< Button is down
    UP  ///< Button is up
  };

  template <std::size_t N>
  int
  alloc_id(std::bitset<N> &gamepad_mask);

  template <std::size_t N>
  void
  free_id(std::bitset<N> &gamepad_mask, int id);

  void
  free_gamepad(platf::input_t &platf_input, int id);

  struct gamepad_t {
    gamepad_t():
        gamepad_state {}, back_timeout_id {}, id { -1 }, back_button_state { button_state_e::NONE } {}
    ~gamepad_t() {
      if (id >= 0) {
        task_pool.push([id = this->id]() {
          free_gamepad(PlatformInput::getInstance(), id);
        });
      }
    }

    platf::gamepad_state_t gamepad_state;

    thread_pool_util::ThreadPool::task_id_t back_timeout_id;

    int id;

    // When emulating the HOME button, we may need to artificially release the back button.
    // Afterwards, the gamepad state on sunshine won't match the state on Moonlight.
    // To prevent Sunshine from sending erroneous input data to the active application,
    // Sunshine forces the button to be in a specific state until the gamepad state matches that of
    // Moonlight once more.
    button_state_e back_button_state;
  };

  void
  print(PNV_MULTI_CONTROLLER_PACKET packet);

  /**
   * @brief Prints a controller arrival packet.
   * @param packet The controller arrival packet.
   */
  void
  print(PSS_CONTROLLER_ARRIVAL_PACKET packet);

  /**
   * @brief Prints a controller touch packet.
   * @param packet The controller touch packet.
   */
  void
  print(PSS_CONTROLLER_TOUCH_PACKET packet);

  /**
   * @brief Prints a controller motion packet.
   * @param packet The controller motion packet.
   */
  void
  print(PSS_CONTROLLER_MOTION_PACKET packet);

  /**
   * @brief Prints a controller battery packet.
   * @param packet The controller battery packet.
   */
  void
  print(PSS_CONTROLLER_BATTERY_PACKET packet);

  /**
   * @brief Called to pass a controller arrival message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller arrival packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet);

  /**
   * @brief Called to pass a controller touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller touch packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_TOUCH_PACKET packet);

  /**
   * @brief Called to pass a controller motion message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller motion packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_MOTION_PACKET packet);

  void
  passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet);

  /**
   * @brief Called to pass a controller battery message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller battery packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_BATTERY_PACKET packet);

  /**
   * @brief Batch two controller touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src);

  /**
   * @brief Batch two controller state messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src);

  /**
   * @brief Batch two controller motion messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src);
}  // namespace input::gamepad
