/**
 * @file src/input/common.h
 * @brief Declarations for common input.
 */
#pragma once

#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <functional>

#include "src/globals.h"
#include "src/platform/common.h"
#include "src/thread_pool.h"

#include "src/input/gamepad.h"

namespace input {

  struct gamepad_orchestrator {
    gamepad_orchestrator():
        gamepads(gamepad::MAX_GAMEPADS) {};

    std::vector<gamepad::gamepad_t> gamepads;
  };

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit
    operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Convert a little-endian netfloat to a native endianness float and clamps it.
   * @param f Netfloat value.
   * @param min The minimium value for clamping.
   * @param max The maximum value for clamping.
   * @return Clamped native endianess float value.
   */
  float
  from_clamped_netfloat(netfloat f, float min, float max);

  /**
   * @brief Convert a little-endian netfloat to a native endianness float.
   * @param f Netfloat value.
   * @return The native endianness float value.
   */
  float
  from_netfloat(netfloat f);

  /**
   * @brief Multiply a polar coordinate pair by a cartesian scaling factor.
   * @param r The radial coordinate.
   * @param angle The angular coordinate (radians).
   * @param scalar The scalar cartesian coordinate pair.
   * @return The scaled radial coordinate.
   */
  float
  multiply_polar_by_cartesian_scalar(float r, float angle, const std::pair<float, float> &scalar);

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float>
  scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);

}  // namespace input
