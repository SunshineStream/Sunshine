/**
 * @file src/input/touch.h
 * @brief Declarations for common touch input.
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
#include "src/thread_pool.h"

namespace input::touch {

  /**
   * @brief Prints a touch packet.
   * @param packet The touch packet.
   */
  void
  print(PSS_TOUCH_PACKET packet);

  /**
   * @brief Convert client coordinates on the specified surface into screen coordinates.
   * @param input The input context.
   * @param val The cartesian coordinate pair to convert.
   * @param size The size of the client's surface containing the value.
   * @return The host-relative coordinate pair if a touchport is available.
   */
  std::optional<std::pair<float, float>>
  client_to_touchport(std::shared_ptr<input_t> &input, const std::pair<float, float> &val, const std::pair<float, float> &size);

  /**
   * @brief Called to pass a touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The touch packet.
   */
  void
  passthrough(std::shared_ptr<input_t> &input, PSS_TOUCH_PACKET packet);

  /**
   * @brief Batch two touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e
  batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src);
}  // namespace input::touch
