/**
 * @file src/text_context.h
 * @brief Correlate completed pointer gestures with Windows editable focus.
 * @details Uses the remote text context v1 wire format introduced in
 * https://github.com/AlkaidLab/foundation-sunshine/pull/1029.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace text_context {
  using clock_t = std::chrono::steady_clock;
  using session_id = std::uint64_t;
  using packet_t = std::array<std::uint8_t, 76>;  ///< Foundation text context v1 payload, little endian.
  constexpr std::uint32_t host_flag = 0x200;  ///< x-ss-general.featureFlags.
  constexpr std::uint32_t client_flag = 0x10;  ///< x-ml-general.featureFlags.
  constexpr std::uint16_t message_type = 0x550c;  ///< Reliable host-to-client control message.
  constexpr std::uint64_t mouse_pointer = 1ULL << 32;  ///< Distinct from every 32-bit touch ID.

  /** @brief Physical pixel bounds, with exclusive right and bottom edges. */
  struct rect_t {
    int left, top, right, bottom;
    bool operator==(const rect_t &) const = default;
    bool contains(int x, int y) const;
  };

  /** @brief Geometry and non-content metadata of a confirmed focused editor. */
  struct editor_t {
    rect_t bounds;
    std::optional<rect_t> caret;
    bool password;
  };

  /** @brief Events accepted by the gesture correlator. */
  enum class action_e {
    down,
    move,
    up,
    cancel,
    cancel_touches
  };

  /** @brief Stateful correlation shared by all subscribers to one desktop. Caller serializes access. */
  class router_t {
  public:
    /** @brief Start tracking a newly negotiated transport connection. */
    void subscribe(session_id id);
    /** @brief Drop all pending and published state belonging to this connection. */
    void unsubscribe(session_id id);
    /** @brief Record one input transition. True requests a fresh UIA snapshot. */
    bool pointer(session_id id, std::uint64_t pointer_id, action_e action, int x, int y, rect_t capture, clock_t::time_point now);

    /** @brief Fence each provider query against concurrent gestures and disconnects. */
    std::uint64_t generation() const {
      return epoch;
    }

    /** @brief Whether an observation can affect a recent gesture or published focus. */
    bool interested(clock_t::time_point now) const;
    /** @brief Route the current editor snapshot if it is still relevant to this generation. */
    void observe(std::uint64_t generation, std::optional<editor_t> editor, clock_t::time_point now);
    /** @brief Move the queued v1 updates out without allocating on empty polls. */
    std::vector<packet_t> drain(session_id id);

    /** @brief Whether the last subscribed connection has gone away. */
    bool empty() const {
      return sessions.empty();
    }

  private:
    struct gesture_t {
      std::uint64_t pointer;
      int x, y;
      rect_t capture;
      clock_t::time_point time;
      bool cancelled {};  ///< Motion, display changes or a second touch disqualified this press.
    };

    struct session_t {
      std::map<std::uint64_t, gesture_t> pressed;
      std::optional<gesture_t> pending;
      std::vector<packet_t> outgoing;
      std::uint32_t revision {};
      std::uint64_t activation {};
    };

    void release_owner();
    void publish(session_t &session, const gesture_t &gesture, const editor_t *editor);
    std::map<session_id, session_t> sessions;
    std::optional<session_id> owner;
    std::optional<gesture_t> active_gesture;
    std::optional<rect_t> active_bounds;
    std::uint64_t epoch {};
  };
}  // namespace text_context
