/**
 * @file src/input/touch.cpp
 * @brief Definitions for common touch input.
 */
#include "src/input/touch.h"
#include "src/input/init.h"
#include "src/input/processor.h"

namespace input::touch {
  void
  print(PSS_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin touch packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "--end touch packet--"sv;
  }

  std::optional<std::pair<float, float>>
  client_to_touchport(std::shared_ptr<input_t> &input, const std::pair<float, float> &val, const std::pair<float, float> &size) {
    auto &touch_port_event = input->touch_port_event;
    auto &touch_port = input->touch_port;
    if (touch_port_event->peek()) {
      touch_port = *touch_port_event->pop();
    }
    if (!touch_port) {
      BOOST_LOG(verbose) << "Ignoring early absolute input without a touch port"sv;
      return std::nullopt;
    }

    auto scalarX = touch_port.width / size.first;
    auto scalarY = touch_port.height / size.second;

    float x = std::clamp(val.first, 0.0f, size.first) * scalarX;
    float y = std::clamp(val.second, 0.0f, size.second) * scalarY;

    auto offsetX = touch_port.client_offsetX;
    auto offsetY = touch_port.client_offsetY;

    x = std::clamp(x, offsetX, (size.first * scalarX) - offsetX);
    y = std::clamp(y, offsetY, (size.second * scalarY) - offsetY);

    return std::pair { (x - offsetX) * touch_port.scalar_inv, (y - offsetY) * touch_port.scalar_inv };
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_TOUCH_PACKET packet) {
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

    platf::touch_input_t touch {
      packet->eventType,
      rotation,
      util::endian::little(packet->pointerId),
      coords->first,
      coords->second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      contact_area.first,
      contact_area.second,
    };

    platf::touch_update(input->client_context.get(), abs_port, touch);
  }

  batch_result_e
  batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
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
}  // namespace input::touch
