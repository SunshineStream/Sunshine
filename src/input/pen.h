/**
 * @file src/input/pen.h
 * @brief Declarations for common pen input.
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

namespace input::pen {
  /**
   * @brief Prints a pen packet.
   * @param packet The pen packet.
   */
  void
  print(PSS_PEN_PACKET packet);

  /**
   * @brief Called to pass a pen message to the platform backend.
   * @param input The input context pointer.
   * @param packet The pen packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_PEN_PACKET packet);

  /**
   * @brief Batch two pen messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src);
}  // namespace input::pen
