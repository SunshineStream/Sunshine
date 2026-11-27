/**
 * @file src/platform/macos/mouse_utils.h
 * @brief Declarations for macOS relative pointer math helpers.
 */
#pragma once

// system includes
#include <CoreGraphics/CoreGraphics.h>

// standard includes
#include <algorithm>
#include <optional>
#include <vector>

namespace platf {
  namespace macos {
    namespace mouse {

      /**
       * @brief A display rectangle in global CoreGraphics point space.
       */
      struct rect_t {
        double x;
        double y;
        double width;
        double height;
      };

      /**
       * @brief The outer bounds of the entire display layout.
       */
      struct bounds_t {
        double min_x;
        double min_y;
        double max_x;
        double max_y;
      };

      /**
       * @brief Compute the union of every active display.
       * @param rects The active display rectangles.
       * @return The layout bounds, or an empty optional when there are no displays.
       */
      inline std::optional<bounds_t>
        layout_bounds(const std::vector<rect_t> &rects) {
        if (rects.empty()) {
          return std::nullopt;
        }

        bounds_t bounds {rects.front().x, rects.front().y, rects.front().x + rects.front().width, rects.front().y + rects.front().height};
        for (const auto &rect : rects) {
          bounds.min_x = std::min(bounds.min_x, rect.x);
          bounds.min_y = std::min(bounds.min_y, rect.y);
          bounds.max_x = std::max(bounds.max_x, rect.x + rect.width);
          bounds.max_y = std::max(bounds.max_y, rect.y + rect.height);
        }

        return bounds;
      }

      /**
       * @brief Clamp a point into the display layout.
       *
       * Secondary displays commonly have negative origins, so the whole layout is clamped
       * instead of the main display rectangle.
       *
       * @param point The point to clamp.
       * @param bounds The bounds to clamp to.
       * @return The clamped point.
       */
      inline CGPoint
        clamp_to_layout(CGPoint point, const bounds_t &bounds) {
        // Keep one point inside the layout so a drag near the edge still has a hit target.
        const double max_x = bounds.max_x - 1.0;
        const double max_y = bounds.max_y - 1.0;

        return {
          std::min(std::max(point.x, bounds.min_x), max_x),
          std::min(std::max(point.y, bounds.min_y), max_y)
        };
      }

      /**
       * @brief Resolve the move event type for the currently held mouse buttons.
       * @param left True when the left button is held.
       * @param right True when the right button is held.
       * @param center True when the center button is held.
       * @return The CoreGraphics event type describing the move.
       */
      inline CGEventType
        move_event_type(bool left, bool right, bool center) {
        if (left) {
          return kCGEventLeftMouseDragged;
        }
        if (right) {
          return kCGEventRightMouseDragged;
        }
        if (center) {
          return kCGEventOtherMouseDragged;
        }

        return kCGEventMouseMoved;
      }

      /**
       * @brief Resolve the button reported alongside a move event.
       * @param type The event type returned by `move_event_type()`.
       * @return The mouse button for `CGEventCreateMouseEvent()`.
       */
      inline CGMouseButton
        move_event_button(CGEventType type) {
        switch (type) {
          case kCGEventRightMouseDragged:
            return kCGMouseButtonRight;
          case kCGEventOtherMouseDragged:
            return kCGMouseButtonCenter;
          default:
            return kCGMouseButtonLeft;
        }
      }

    }  // namespace mouse
  }  // namespace macos

  namespace macos {
    /**
     * @brief Inject a relative pointer movement through CoreGraphics.
     *
     * A single pointer update is emitted per movement so that the delta cannot be applied
     * twice, or fought over by two mechanisms.
     *
     * @param delta_x The horizontal movement in pointer units.
     * @param delta_y The vertical movement in pointer units.
     * @return True when the movement was injected, false when CoreGraphics cannot be used.
     */
    bool move_mouse_relative(int delta_x, int delta_y);
  }  // namespace macos

}  // namespace platf
