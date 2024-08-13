/**
 * @file src/input/mouse.cpp
 * @brief Definitions for common mouse input.
 */
#include "mouse.h"

#include "src/input/init.h"
#include "src/input/platform_input.h"
#include "src/input/processor.h"
#include "src/input/touch.h"

namespace input::mouse {

#define DISABLE_LEFT_BUTTON_DELAY ((thread_pool_util::ThreadPool::task_id_t) 0x01)
#define ENABLE_LEFT_BUTTON_DELAY nullptr

  static std::array<std::uint8_t, 5> mouse_press {};

  void
  print(PNV_REL_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin relative mouse move packet--"sv << std::endl
      << "deltaX ["sv << util::endian::big(packet->deltaX) << ']' << std::endl
      << "deltaY ["sv << util::endian::big(packet->deltaY) << ']' << std::endl
      << "--end relative mouse move packet--"sv;
  }

  void
  print(PNV_ABS_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin absolute mouse move packet--"sv << std::endl
      << "x      ["sv << util::endian::big(packet->x) << ']' << std::endl
      << "y      ["sv << util::endian::big(packet->y) << ']' << std::endl
      << "width  ["sv << util::endian::big(packet->width) << ']' << std::endl
      << "height ["sv << util::endian::big(packet->height) << ']' << std::endl
      << "--end absolute mouse move packet--"sv;
  }

  void
  print(PNV_MOUSE_BUTTON_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse button packet--"sv << std::endl
      << "action ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "button ["sv << util::hex(packet->button).to_string_view() << ']' << std::endl
      << "--end mouse button packet--"sv;
  }

  void
  print(PNV_SCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse scroll packet--"sv << std::endl
      << "scrollAmt1 ["sv << util::endian::big(packet->scrollAmt1) << ']' << std::endl
      << "--end mouse scroll packet--"sv;
  }

  void
  print(PSS_HSCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse hscroll packet--"sv << std::endl
      << "scrollAmount ["sv << util::endian::big(packet->scrollAmount) << ']' << std::endl
      << "--end mouse hscroll packet--"sv;
  }

  void
  passthrough(const std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    input->mouse_left_button_timeout = DISABLE_LEFT_BUTTON_DELAY;
    platf::move_mouse(PlatformInput::getInstance(), util::endian::big(packet->deltaX), util::endian::big(packet->deltaY));
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PNV_ABS_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (input->mouse_left_button_timeout == DISABLE_LEFT_BUTTON_DELAY) {
      input->mouse_left_button_timeout = ENABLE_LEFT_BUTTON_DELAY;
    }

    float x = util::endian::big(packet->x);
    float y = util::endian::big(packet->y);

    // Prevent divide by zero
    // Don't expect it to happen, but just in case
    if (!packet->width || !packet->height) {
      BOOST_LOG(warning) << "Moonlight passed invalid dimensions"sv;

      return;
    }

    auto width = (float) util::endian::big(packet->width);
    auto height = (float) util::endian::big(packet->height);

    auto tpcoords = touch::client_to_touchport(input, { x, y }, { width, height });
    if (!tpcoords) {
      return;
    }

    auto &touch_port = input->touch_port;
    platf::touch_port_t abs_port {
      touch_port.offset_x, touch_port.offset_y,
      touch_port.env_width, touch_port.env_height
    };

    platf::abs_mouse(PlatformInput::getInstance(), abs_port, tpcoords->first, tpcoords->second);
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5;
    auto button = util::endian::big(packet->button);
    if (button > 0 && button < mouse_press.size()) {
      if (mouse_press[button] != release) {
        // button state is already what we want
        return;
      }

      mouse_press[button] = !release;
    }

    /**
     * When Moonlight sends mouse input through absolute coordinates,
     * it's possible that BUTTON_RIGHT is pressed down immediately after releasing BUTTON_LEFT.
     * As a result, Sunshine will left-click on hyperlinks in the browser before right-clicking
     *
     * This can be solved by delaying BUTTON_LEFT, however, any delay on input is undesirable during gaming
     * As a compromise, Sunshine will only put delays on BUTTON_LEFT when
     * absolute mouse coordinates have been sent.
     *
     * Try to make sure BUTTON_RIGHT gets called before BUTTON_LEFT is released.
     *
     * input->mouse_left_button_timeout can only be nullptr
     * when the last mouse coordinates were absolute
     */
    if (button == BUTTON_LEFT && release && !input->mouse_left_button_timeout) {
      auto f = [=]() {
        auto left_released = mouse_press[BUTTON_LEFT];
        if (left_released) {
          // Already released left button
          return;
        }
        platf::button_mouse(PlatformInput::getInstance(), BUTTON_LEFT, release);

        mouse_press[BUTTON_LEFT] = false;
        input->mouse_left_button_timeout = nullptr;
      };

      input->mouse_left_button_timeout = task_pool.pushDelayed(std::move(f), 10ms).task_id;

      return;
    }
    if (
      button == BUTTON_RIGHT && !release &&
      input->mouse_left_button_timeout > DISABLE_LEFT_BUTTON_DELAY) {
      platf::button_mouse(PlatformInput::getInstance(), BUTTON_RIGHT, false);
      platf::button_mouse(PlatformInput::getInstance(), BUTTON_RIGHT, true);

      mouse_press[BUTTON_RIGHT] = false;

      return;
    }

    platf::button_mouse(PlatformInput::getInstance(), button, release);
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling) {
      platf::scroll(PlatformInput::getInstance(), util::endian::big(packet->scrollAmt1));
    }
    else {
      input->accumulated_vscroll_delta += util::endian::big(packet->scrollAmt1);
      if (auto full_ticks = input->accumulated_vscroll_delta / WHEEL_DELTA) {
        // Send any full ticks that have accumulated and store the rest
        platf::scroll(PlatformInput::getInstance(), full_ticks * WHEEL_DELTA);
        input->accumulated_vscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  void
  passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling) {
      platf::hscroll(PlatformInput::getInstance(), util::endian::big(packet->scrollAmount));
    }
    else {
      input->accumulated_hscroll_delta += util::endian::big(packet->scrollAmount);
      if (auto full_ticks = input->accumulated_hscroll_delta / WHEEL_DELTA) {
        // Send any full ticks that have accumulated and store the rest
        platf::hscroll(PlatformInput::getInstance(), full_ticks * WHEEL_DELTA);
        input->accumulated_hscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  batch_result_e
  batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src) {
    short deltaX, deltaY;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->deltaX), util::endian::big(src->deltaX), &deltaX)) {
      return batch_result_e::terminate_batch;
    }
    if (!__builtin_add_overflow(util::endian::big(dest->deltaY), util::endian::big(src->deltaY), &deltaY)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of deltas
    dest->deltaX = util::endian::big(deltaX);
    dest->deltaY = util::endian::big(deltaY);
    return batch_result_e::batched;
  }

  batch_result_e
  batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src) {
    // Batching must only happen if the reference width and height don't change
    if (dest->width != src->width || dest->height != src->height) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest absolute position
    *dest = *src;
    return batch_result_e::batched;
  }

  batch_result_e
  batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->scrollAmt1), util::endian::big(src->scrollAmt1), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmt1 = util::endian::big(scrollAmt);
    dest->scrollAmt2 = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  batch_result_e
  batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->scrollAmount), util::endian::big(src->scrollAmount), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmount = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  void
  reset(platf::input_t &platf_input) {
    for (int x = 0; x < mouse_press.size(); ++x) {
      if (mouse_press[x]) {
        platf::button_mouse(platf_input, x, true);
        mouse_press[x] = false;
      }
    }
  }

  void
  force_frame_move(platf::input_t &platf_input) {
    task_pool.pushDelayed([&platf_input]() {
      platf::move_mouse(platf_input, 1, 1);
      platf::move_mouse(platf_input, -1, -1);
    },
      100ms);
  }

  void
  cancel(const std::shared_ptr<input_t> &input) {
    task_pool.cancel(input->mouse_left_button_timeout);
  }
}  // namespace input::mouse
