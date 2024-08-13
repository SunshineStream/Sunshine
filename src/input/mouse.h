/**
 * @file src/input/mouse.h
 * @brief Declarations for common mouse input.
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

namespace input::mouse {
// Win32 WHEEL_DELTA constant
#ifndef WHEEL_DELTA
  #define WHEEL_DELTA 120
#endif

  /**
   * @brief Prints a relative mouse movement event (x, y) packet.
   * @param packet The move packet.
   */
  void
  print(PNV_REL_MOUSE_MOVE_PACKET packet);

  /**
   * @brief Prints an absolute mouse movement event (x, y) packet.
   * @param packet The move packet.
   */
  void
  print(PNV_ABS_MOUSE_MOVE_PACKET packet);

  /**
   * @brief Prints a mouse button event packet.
   * @param packet The button packet.
   */
  void
  print(PNV_MOUSE_BUTTON_PACKET packet);

  /**
   * @brief Prints a vertical mouse scroll event packet.
   * @param packet The scroll packet.
   */
  void
  print(PNV_SCROLL_PACKET packet);

  /**
   * @brief Prints a horizontal mouse scroll event packet.
   * @param packet The scroll packet.
   */
  void
  print(PSS_HSCROLL_PACKET packet);

  /**
   * @brief Called to pass a relative mouse movement event (dx, dy) to the platform backend.
   * @param input The input context pointer.
   * @param packet The move event packet.
   */
  void
  passthrough(const std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet);

  /**
   * @brief Called to pass an absolute mouse movement event (x, y) to the platform backend.
   * @param input The input context pointer.
   * @param packet The move event packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PNV_ABS_MOUSE_MOVE_PACKET packet);

  /**
   * @brief Called to pass a button event message to the platform backend.
   * @param input The input context pointer.
   * @param packet The button event packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet);

  /**
   * @brief Called to pass a vertical scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet);

  /**
   * @brief Called to pass a horizontal scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet);

  /**
   * @brief Batch two relative mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src);

  /**
   * @brief Batch two absolute mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src);

  /**
   * @brief Batch two vertical scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src);

  /**
   * @brief Batch two horizontal scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src);

  /**
   * @brief Resets the overall state of a mouse in the platform backend.
   * @param platf_input The input context reference.
   */
  void
  reset(platf::input_t &platf_input);

  /**
   * @brief Move the mouse slightly to force a video frame render
   * @param platf_input
   */
  void
  force_frame_move(platf::input_t &platf_input);

  /**
   * @brief Cancel pending mouse events in the platform backend task pool.
   */
  void
  cancel(const std::shared_ptr<input_t> &input);
}  // namespace input::mouse
