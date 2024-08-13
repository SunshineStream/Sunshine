/**
 * @file src/input/pen.cpp
 * @brief Definitions for common pen input.
 */
#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <bitset>
#include <chrono>
#include <cmath>
#include <list>
#include <thread>
#include <unordered_map>

#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/thread_pool.h"
#include "src/utility.h"

#include "src/input/common.h"
#include "src/input/init.h"
#include "src/input/pen.h"
#include "src/input/processor.h"
#include "src/input/touch.h"
#include <boost/endian/buffers.hpp>

using namespace std::literals;

namespace input::pen {
  void
  print(PSS_PEN_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin pen packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "toolType ["sv << util::hex(packet->toolType).to_string_view() << ']' << std::endl
      << "penButtons ["sv << util::hex(packet->penButtons).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "tilt ["sv << (uint32_t) packet->tilt << ']' << std::endl
      << "--end pen packet--"sv;
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_PEN_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    // Convert the client normalized coordinates to touchport coordinates
    auto coords = touch::client_to_touchport(input,
      { from_clamped_netfloat(packet->x, 0.0f, 1.0f) * 65535.f,
        from_clamped_netfloat(packet->y, 0.0f, 1.0f) * 65535.f },
      { 65535.f, 65535.f });
    if (!coords) {
      return;
    }

    auto &touch_port = input->touch_port;
    platf::touch_port_t abs_port {
      touch_port.offset_x, touch_port.offset_y,
      touch_port.env_width, touch_port.env_height
    };

    // Renormalize the coordinates
    coords->first /= abs_port.width;
    coords->second /= abs_port.height;

    // Normalize rotation value to 0-359 degree range
    auto rotation = util::endian::little(packet->rotation);
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    // Normalize the contact area based on the touchport
    auto contact_area = scale_client_contact_area(
      { from_clamped_netfloat(packet->contactAreaMajor, 0.0f, 1.0f) * 65535.f,
        from_clamped_netfloat(packet->contactAreaMinor, 0.0f, 1.0f) * 65535.f },
      rotation,
      { abs_port.width / 65535.f, abs_port.height / 65535.f });

    platf::pen_input_t pen {
      packet->eventType,
      packet->toolType,
      packet->penButtons,
      packet->tilt,
      rotation,
      coords->first,
      coords->second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      contact_area.first,
      contact_area.second,
    };

    platf::pen_update(input->client_context.get(), abs_port, pen);
  }

  batch_result_e
  batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same type
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Do not allow batching if the button state changes
    if (dest->penButtons != src->penButtons) {
      return batch_result_e::terminate_batch;
    }

    // Do not batch beyond tool changes
    if (dest->toolType != src->toolType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

}  // namespace input::pen
