/**
 * @file src/input/processor.cpp
 * @brief Definitions for common processor input.
 */
// define uint32_t for <moonlight-common-c/src/Input.h>

#include "platform_input.h"

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

#include "src/input/common.h"
#include "src/input/gamepad.h"
#include "src/input/init.h"
#include "src/input/keyboard.h"
#include "src/input/mouse.h"
#include "src/input/pen.h"
#include "src/input/processor.h"
#include "src/input/touch.h"

#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/thread_pool.h"
#include "src/utility.h"

#include <boost/endian/buffers.hpp>

using namespace std::literals;

namespace input {

  namespace {
    /**
     * @brief Batch two input messages.
     * @param dest The original packet to batch into.
     * @param src A later packet to attempt to batch.
     * @return The status of the batching operation.
     */
    batch_result_e
    batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src) {
      // We can only batch if the packet types are the same
      if (dest->magic != src->magic) {
        return batch_result_e::terminate_batch;
      }

      // We can only batch certain message types
      switch (util::endian::little(dest->magic)) {
        case MOUSE_MOVE_REL_MAGIC_GEN5:
          return mouse::batch((PNV_REL_MOUSE_MOVE_PACKET) dest, (PNV_REL_MOUSE_MOVE_PACKET) src);
        case MOUSE_MOVE_ABS_MAGIC:
          return mouse::batch((PNV_ABS_MOUSE_MOVE_PACKET) dest, (PNV_ABS_MOUSE_MOVE_PACKET) src);
        case SCROLL_MAGIC_GEN5:
          return mouse::batch((PNV_SCROLL_PACKET) dest, (PNV_SCROLL_PACKET) src);
        case SS_HSCROLL_MAGIC:
          return mouse::batch((PSS_HSCROLL_PACKET) dest, (PSS_HSCROLL_PACKET) src);
        case MULTI_CONTROLLER_MAGIC_GEN5:
          return gamepad::batch((PNV_MULTI_CONTROLLER_PACKET) dest, (PNV_MULTI_CONTROLLER_PACKET) src);
        case SS_TOUCH_MAGIC:
          return touch::batch((PSS_TOUCH_PACKET) dest, (PSS_TOUCH_PACKET) src);
        case SS_PEN_MAGIC:
          return pen::batch((PSS_PEN_PACKET) dest, (PSS_PEN_PACKET) src);
        case SS_CONTROLLER_TOUCH_MAGIC:
          return gamepad::batch((PSS_CONTROLLER_TOUCH_PACKET) dest, (PSS_CONTROLLER_TOUCH_PACKET) src);
        case SS_CONTROLLER_MOTION_MAGIC:
          return gamepad::batch((PSS_CONTROLLER_MOTION_PACKET) dest, (PSS_CONTROLLER_MOTION_PACKET) src);
        default:
          // Not a batchable message type
          return batch_result_e::terminate_batch;
      }
    }

    /**
     * @brief Called on a thread pool thread to process an input message.
     * @param input The input context pointer.
     */
    void
    passthrough_next_message(std::shared_ptr<input_t> input) {
      // 'entry' backs the 'payload' pointer, so they must remain in scope together
      std::vector<uint8_t> entry;
      PNV_INPUT_HEADER payload;

      // Lock the input queue while batching, but release it before sending
      // the input to the OS. This avoids potentially lengthy lock contention
      // in the control stream thread while input is being processed by the OS.
      {
        std::lock_guard<std::mutex> lg(input->input_queue_lock);

        // If all entries have already been processed, nothing to do
        if (input->input_queue.empty()) {
          return;
        }

        // Pop off the first entry, which we will send
        entry = input->input_queue.front();
        payload = (PNV_INPUT_HEADER) entry.data();
        input->input_queue.pop_front();

        // Try to batch with remaining items on the queue
        auto i = input->input_queue.begin();
        while (i != input->input_queue.end()) {
          auto batchable_entry = *i;
          auto batchable_payload = (PNV_INPUT_HEADER) batchable_entry.data();

          auto batch_result = batch(payload, batchable_payload);
          if (batch_result == batch_result_e::terminate_batch) {
            // Stop batching
            break;
          }
          else if (batch_result == batch_result_e::batched) {
            // Erase this entry since it was batched
            i = input->input_queue.erase(i);
          }
          else {
            // We couldn't batch this entry, but try to batch later entries.
            i++;
          }
        }
      }

      // Print the final input packet
      input::print((void *) payload);

      // Send the batched input to the OS
      switch (util::endian::little(payload->magic)) {
        case MOUSE_MOVE_REL_MAGIC_GEN5:
          mouse::passthrough(input, (PNV_REL_MOUSE_MOVE_PACKET) payload);
          break;
        case MOUSE_MOVE_ABS_MAGIC:
          mouse::passthrough(input, (PNV_ABS_MOUSE_MOVE_PACKET) payload);
          break;
        case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
        case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
          mouse::passthrough(input, (PNV_MOUSE_BUTTON_PACKET) payload);
          break;
        case SCROLL_MAGIC_GEN5:
          mouse::passthrough(input, (PNV_SCROLL_PACKET) payload);
          break;
        case SS_HSCROLL_MAGIC:
          mouse::passthrough(input, (PSS_HSCROLL_PACKET) payload);
          break;
        case KEY_DOWN_EVENT_MAGIC:
        case KEY_UP_EVENT_MAGIC:
          keyboard::passthrough(input, (PNV_KEYBOARD_PACKET) payload);
          break;
        case UTF8_TEXT_EVENT_MAGIC:
          keyboard::passthrough((PNV_UNICODE_PACKET) payload);
          break;
        case MULTI_CONTROLLER_MAGIC_GEN5:
          gamepad::passthrough(input, (PNV_MULTI_CONTROLLER_PACKET) payload);
          break;
        case SS_TOUCH_MAGIC:
          touch::passthrough(input, (PSS_TOUCH_PACKET) payload);
          break;
        case SS_PEN_MAGIC:
          pen::passthrough(input, (PSS_PEN_PACKET) payload);
          break;
        case SS_CONTROLLER_ARRIVAL_MAGIC:
          gamepad::passthrough(input, (PSS_CONTROLLER_ARRIVAL_PACKET) payload);
          break;
        case SS_CONTROLLER_TOUCH_MAGIC:
          gamepad::passthrough(input, (PSS_CONTROLLER_TOUCH_PACKET) payload);
          break;
        case SS_CONTROLLER_MOTION_MAGIC:
          gamepad::passthrough(input, (PSS_CONTROLLER_MOTION_PACKET) payload);
          break;
        case SS_CONTROLLER_BATTERY_MAGIC:
          gamepad::passthrough(input, (PSS_CONTROLLER_BATTERY_PACKET) payload);
          break;
      }
    }
  }  // namespace

  void
  print(void *payload) {
    const auto header = static_cast<PNV_INPUT_HEADER>(payload);

    switch (util::endian::little(header->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        mouse::print(static_cast<PNV_REL_MOUSE_MOVE_PACKET>(payload));
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        mouse::print(static_cast<PNV_ABS_MOUSE_MOVE_PACKET>(payload));
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        mouse::print(static_cast<PNV_MOUSE_BUTTON_PACKET>(payload));
        break;
      case SCROLL_MAGIC_GEN5:
        mouse::print(static_cast<PNV_SCROLL_PACKET>(payload));
        break;
      case SS_HSCROLL_MAGIC:
        mouse::print(static_cast<PSS_HSCROLL_PACKET>(payload));
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        keyboard::print(static_cast<PNV_KEYBOARD_PACKET>(payload));
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        keyboard::print(static_cast<PNV_UNICODE_PACKET>(payload));
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        gamepad::print(static_cast<PNV_MULTI_CONTROLLER_PACKET>(payload));
        break;
      case SS_TOUCH_MAGIC:
        touch::print(static_cast<PSS_TOUCH_PACKET>(payload));
        break;
      case SS_PEN_MAGIC:
        pen::print(static_cast<PSS_PEN_PACKET>(payload));
        break;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        gamepad::print(static_cast<PSS_CONTROLLER_ARRIVAL_PACKET>(payload));
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        gamepad::print(static_cast<PSS_CONTROLLER_TOUCH_PACKET>(payload));
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        gamepad::print(static_cast<PSS_CONTROLLER_MOTION_PACKET>(payload));
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        gamepad::print(static_cast<PSS_CONTROLLER_BATTERY_PACKET>(payload));
        break;
    }
  }

  void
  passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);
      input->input_queue.push_back(std::move(input_data));
    }
    task_pool.push(passthrough_next_message, input);
  }

  void
  reset(std::shared_ptr<input_t> &input) {
    keyboard::cancel();
    mouse::cancel(input);

    // Ensure input is synchronous, by using the task_pool
    task_pool.push([]() {
      mouse::reset(PlatformInput::getInstance());
      keyboard::reset(PlatformInput::getInstance());
    });
  }

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() override {
      PlatformInput::getInstance().reset();
    }
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t>
  init() {
    PlatformInput::_instance = platf::input();

    return std::make_unique<deinit_t>();
  }

  bool
  probe_gamepads() {
    platf::input_t &input = PlatformInput::getInstance();
    const auto gamepads = platf::supported_gamepads(reinterpret_cast<platf::input_t *>(&input));
    for (auto &gamepad : gamepads) {
      if (gamepad.is_enabled && gamepad.name != "auto") {
        return false;
      }
    }
    return true;
  }

  std::shared_ptr<input_t>
  alloc(safe::mail_t mail) {
    auto input = std::make_shared<input_t>(
      mail->event<input::touch_port_t>(mail::touch_port),
      mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback));

    // Workaround to ensure new frames will be captured when a client connects
    mouse::force_frame_move(PlatformInput::getInstance());
    return input;
  }
}  // namespace input
