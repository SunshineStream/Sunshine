/**
 * @file src/platform/windows/text_context.h
 * @brief Lifetime and input hooks for the user-desktop UI Automation worker.
 */
#pragma once

#include "src/text_context.h"

namespace text_context::windows {
  /** @brief Subscribe an encrypted control connection and start observation on first use. */
  void start(session_id id);
  /** @brief Forget all connection state and stop observing when the last subscriber disconnects. */
  void stop(session_id id);
  /** @brief Record a pointer transition without calling a UI Automation provider on the input thread. */
  void pointer(session_id id, std::uint64_t key, action_e action, int x, int y, rect_t capture);
  /** @brief Consume queued state transitions only for this connection. */
  std::vector<packet_t> drain(session_id id);
}  // namespace text_context::windows
