/**
 * @file src/text_context.cpp
 * @brief Route text focus to the subscriber whose completed gesture hit the editor.
 */
#include "text_context.h"

#include <algorithm>
#include <cmath>

namespace text_context {
  namespace {
    constexpr auto match_window = std::chrono::milliseconds(1200);
    constexpr auto press_lifetime = std::chrono::seconds(30);

    /** @brief Store the low bytes of a field in protocol order, independent of host packing. */
    void put(packet_t &packet, std::size_t offset, std::uint64_t value, unsigned bytes) {
      for (unsigned i = 0; i < bytes; ++i) {
        packet[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
      }
    }
  }  // namespace

  bool rect_t::contains(int x, int y) const {
    return x >= left && x < right && y >= top && y < bottom;
  }

  void router_t::subscribe(session_id id) {
    sessions.try_emplace(id);
    ++epoch;
  }

  void router_t::unsubscribe(session_id id) {
    if (owner == id) {
      owner.reset();
      active_gesture.reset();
      active_bounds.reset();
    }
    sessions.erase(id);
    ++epoch;
  }

  bool router_t::pointer(session_id id, std::uint64_t pointer_id, action_e action, int x, int y, rect_t capture, clock_t::time_point now) {
    const auto it = sessions.find(id);
    if (it == sessions.end()) {
      return false;
    }
    auto &session = it->second;
    auto &pressed = session.pressed;
    if (action == action_e::cancel || action == action_e::cancel_touches) {
      const auto cancelled = [&](std::uint64_t key) {
        return action == action_e::cancel ? key == pointer_id : key != mouse_pointer;
      };
      std::erase_if(pressed, [&](const auto &entry) {
        return cancelled(entry.first);
      });
      if (session.pending && cancelled(session.pending->pointer)) {
        session.pending.reset();
        ++epoch;
      }
      return false;
    }
    if (action == action_e::down) {
      std::erase_if(pressed, [&](const auto &entry) {
        return now - entry.second.time > press_lifetime;
      });
      if (!capture.contains(x, y) || pressed.contains(pointer_id)) {
        return false;
      }
      bool multiple_touches = false;
      if (pointer_id != mouse_pointer) {
        for (auto &[key, gesture] : pressed) {
          if (key != mouse_pointer) {
            multiple_touches = true;
            gesture.cancelled = true;
          }
        }
        if (pressed.size() - pressed.count(mouse_pointer) >= 16) {
          return false;
        }
      }
      pressed.emplace(pointer_id, gesture_t {pointer_id, x, y, capture, now, multiple_touches});
      return false;
    }
    const auto down = pressed.find(pointer_id);
    if (down == pressed.end()) {
      return false;
    }
    const auto dx = static_cast<double>(x) - down->second.x;
    const auto dy = static_cast<double>(y) - down->second.y;
    if (dx * dx + dy * dy > 24 * 24 || capture != down->second.capture || !capture.contains(x, y) || now - down->second.time > press_lifetime) {
      down->second.cancelled = true;
    }
    if (action == action_e::up) {
      const bool cancelled = down->second.cancelled;
      pressed.erase(down);
      if (cancelled) {
        return false;
      }
      session.pending = gesture_t {pointer_id, x, y, capture, now};
      ++epoch;
      return true;
    }
    return false;
  }

  bool router_t::interested(clock_t::time_point now) const {
    return owner || std::any_of(sessions.begin(), sessions.end(), [&](const auto &entry) {
             const auto &pending = entry.second.pending;
             return pending && now - pending->time <= match_window;
           });
  }

  void router_t::observe(std::uint64_t generation, std::optional<editor_t> editor, clock_t::time_point now) {
    if (generation != epoch) {
      return;
    }
    if (owner && (!editor || editor->bounds != active_bounds)) {
      release_owner();
    }
    if (!editor) {
      return;
    }
    std::optional<session_id> match;
    for (auto &[id, session] : sessions) {
      if (session.pending && now - session.pending->time > match_window) {
        session.pending.reset();
      }
      if (session.pending && editor->bounds.contains(session.pending->x, session.pending->y)) {
        if (match) {
          // Discard ambiguous candidates so disconnecting one client cannot credit the other later.
          for (auto &[unused, candidate] : sessions) {
            if (candidate.pending && editor->bounds.contains(candidate.pending->x, candidate.pending->y)) {
              candidate.pending.reset();
            }
          }
          return;
        }
        match = id;
      }
    }
    if (match) {
      if (owner && owner != match) {
        release_owner();
      }
      auto &session = sessions.at(*match);
      owner = match;
      active_bounds = editor->bounds;
      active_gesture = session.pending;
      session.pending.reset();
      ++session.activation;
      publish(session, *active_gesture, &*editor);
    }
  }

  void router_t::release_owner() {
    publish(sessions.at(*owner), *active_gesture, nullptr);
    owner.reset();
    active_gesture.reset();
    active_bounds.reset();
  }

  void router_t::publish(session_t &session, const gesture_t &gesture, const editor_t *editor) {
    packet_t packet {};
    packet[0] = 1;
    packet[1] = packet.size();
    std::uint16_t flags = 0x90;  // INPUT_MATCHED | ANCHOR_POINT
    const auto &capture = gesture.capture;
    const auto local = [&](std::size_t offset, int x, int y) {
      put(packet, offset, static_cast<std::int64_t>(x) - capture.left, 4);
      put(packet, offset + 4, static_cast<std::int64_t>(y) - capture.top, 4);
    };
    const auto rectangle = [&](std::size_t offset, const rect_t &rect) {
      local(offset, std::max(rect.left, capture.left), std::max(rect.top, capture.top));
      local(offset + 8, std::min(rect.right, capture.right), std::min(rect.bottom, capture.bottom));
    };
    if (editor) {
      flags |= 0x23;  // ACTIVE | EDITABLE | ELEMENT_RECT
      if (editor->password) {
        flags |= 0x04;
      }
      rectangle(36, editor->bounds);
      if (editor->caret && capture.contains(editor->caret->left, editor->caret->top) && editor->bounds.contains(editor->caret->left, editor->caret->top)) {
        flags |= 0x40;
        rectangle(52, *editor->caret);
      }
    }
    put(packet, 2, flags, 2);
    put(packet, 4, ++session.revision, 4);
    put(packet, 8, session.activation, 8);
    put(packet, 16, session.activation, 8);  // One input token per successful activation.
    packet[24] = 2;  // UI Automation
    packet[25] = gesture.pointer == mouse_pointer ? 2 : 1;
    local(28, gesture.x, gesture.y);
    local(68, capture.right, capture.bottom);
    // A stalled control connection only needs the most recent state transitions.
    if (session.outgoing.size() == 2) {
      session.outgoing.erase(session.outgoing.begin());
    }
    session.outgoing.push_back(packet);
  }

  std::vector<packet_t> router_t::drain(session_id id) {
    std::vector<packet_t> out;
    if (const auto it = sessions.find(id); it != sessions.end()) {
      out.swap(it->second.outgoing);
    }
    return out;
  }
}  // namespace text_context
