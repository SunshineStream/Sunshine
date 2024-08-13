/**
 * @file src/input/keyboard.h
 * @brief Declarations for common keyboard input.
 */
#pragma once

#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <functional>

#include "src/input/common.h"
#include "src/platform/common.h"
#include "src/thread_safe.h"

namespace input::keyboard {
  typedef uint32_t key_press_id_t;

  /**
   * @brief Prints a keyboard event packet.
   * @param packet The keyboard event packet.
   */
  void
  print(PNV_KEYBOARD_PACKET packet);

  /**
   * @brief Prints a unicode text packet.
   * @param packet The unicode text packet.
   */
  void
  print(PNV_UNICODE_PACKET packet);

  /**
   * @brief Called to pass a keyboard event to the platform backend.
   * @param input The input context pointer.
   * @param packet The keyboard event packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet);

  /**
   * @brief Called to pass a unicode text message to the platform backend.
   * @param packet The unicode text packet.
   */
  void
  passthrough(PNV_UNICODE_PACKET packet);

  /**
   * @brief Resets the overall state of a keyboard in the platform backend.
   * @param platf_input The input context reference.
   */
  void
  reset(platf::input_t &platf_input);

  /**
   * @brief Cancel pending key presses in the platform backend task pool.
   */
  void
  cancel();
}  // namespace input::keyboard
