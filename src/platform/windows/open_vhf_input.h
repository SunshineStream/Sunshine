// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "src/platform/common.h"

#include <memory>
#include <string_view>

namespace platf {
  class open_vhf_input_t {
  public:
    open_vhf_input_t();
    ~open_vhf_input_t();

    bool available(std::string_view profile);
    bool allocate(const gamepad_id_t &id, const gamepad_arrival_t &arrival, const feedback_queue_t &queue);
    bool owns(int index);
    int rebind(const gamepad_id_t &id, feedback_queue_t queue);
    void destroy(int index);
    void update(int index, const gamepad_state_t &state);
    void touch(const gamepad_touch_t &event);
    void motion(const gamepad_motion_t &event);
    void battery(const gamepad_battery_t &event);

  private:
    struct implementation_t;
    std::unique_ptr<implementation_t> implementation;
  };
}
