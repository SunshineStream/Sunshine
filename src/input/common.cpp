/**
 * @file src/input/common.cpp
 * @brief Definitions for common input.
 */
#include "src/input/common.h"
#include "src/input/gamepad.h"
#include "src/input/init.h"

#include "src/thread_pool.h"

#include <boost/endian/buffers.hpp>

using namespace std::literals;

namespace input {

  float
  from_netfloat(netfloat f) {
    return boost::endian::endian_load<float, sizeof(float), boost::endian::order::little>(f);
  }

  float
  from_clamped_netfloat(netfloat f, float min, float max) {
    return std::clamp(from_netfloat(f), min, max);
  }

  float
  multiply_polar_by_cartesian_scalar(float r, float angle, const std::pair<float, float> &scalar) {
    // Convert polar to cartesian coordinates
    float x = r * std::cos(angle);
    float y = r * std::sin(angle);

    // Scale the values
    x *= scalar.first;
    y *= scalar.second;

    // Convert the result back to a polar radial coordinate
    return std::sqrt(std::pow(x, 2) + std::pow(y, 2));
  }

  std::pair<float, float>
  scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar) {
    // If the rotation is unknown, we'll just scale both axes equally by using
    // a 45-degree angle for our scaling calculations
    float angle = rotation == LI_ROT_UNKNOWN ? (M_PI / 4) : (rotation * (M_PI / 180));

    // If we have a major but not a minor axis, treat the touch as circular
    float major = val.first;
    float minor = val.second != 0.0f ? val.second : val.first;

    // The minor axis is perpendicular to major axis so the angle must be rotated by 90 degrees
    return { multiply_polar_by_cartesian_scalar(major, angle, scalar), multiply_polar_by_cartesian_scalar(minor, angle + (M_PI / 2), scalar) };
  }
}  // namespace input
