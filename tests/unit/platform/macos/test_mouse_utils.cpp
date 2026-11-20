/**
 * @file tests/unit/platform/macos/test_mouse_utils.cpp
 * @brief Tests for macOS relative pointer math helpers.
 */

#ifdef __APPLE__

  // system includes
  #include <CoreGraphics/CoreGraphics.h>

  // standard includes
  #include <gtest/gtest.h>
  #include <optional>
  #include <vector>

  // local includes
  #include "src/platform/macos/mouse_utils.h"

using platf::macos::mouse::bounds_t;
using platf::macos::mouse::clamp_to_layout;
using platf::macos::mouse::layout_bounds;
using platf::macos::mouse::move_event_button;
using platf::macos::mouse::move_event_type;
using platf::macos::mouse::rect_t;

namespace {
  std::optional<bounds_t> bounds_of(std::vector<rect_t> rects) {
    return layout_bounds(rects);
  }
}  // namespace

TEST(MacosMouseUtils, LayoutBoundsSpansTheWholeLayout) {
  const auto bounds = bounds_of({{0, 0, 1920, 1080}, {-2560, 40, 2560, 1440}});
  ASSERT_TRUE(bounds.has_value());
  EXPECT_DOUBLE_EQ(-2560.0, bounds->min_x);
  EXPECT_DOUBLE_EQ(0.0, bounds->min_y);
  EXPECT_DOUBLE_EQ(1920.0, bounds->max_x);
  EXPECT_DOUBLE_EQ(1480.0, bounds->max_y);
}

TEST(MacosMouseUtils, LayoutBoundsIsEmptyWithoutDisplays) {
  EXPECT_FALSE(bounds_of({}).has_value());
}

TEST(MacosMouseUtils, ClampKeepsSecondaryDisplaysReachable) {
  // A secondary display to the left has a negative origin: clamping to the main display only
  // would make it unreachable.
  const bounds_t bounds {-2560, 0, 1920, 1480};

  const auto left = clamp_to_layout({-9000, 500}, bounds);
  EXPECT_DOUBLE_EQ(-2560.0, left.x);
  EXPECT_DOUBLE_EQ(500.0, left.y);

  const auto above = clamp_to_layout({100, -700}, bounds);
  EXPECT_DOUBLE_EQ(100.0, above.x);
  EXPECT_DOUBLE_EQ(0.0, above.y);
}

TEST(MacosMouseUtils, ClampStopsJustInsideTheOuterEdge) {
  const bounds_t bounds {0, 0, 1920, 1080};

  const auto right = clamp_to_layout({5000, 500}, bounds);
  EXPECT_DOUBLE_EQ(1919.0, right.x);

  const auto below = clamp_to_layout({500, 5000}, bounds);
  EXPECT_DOUBLE_EQ(1079.0, below.y);
}

TEST(MacosMouseUtils, ClampPreservesFractionalPoints) {
  const bounds_t bounds {0, 0, 1920, 1080};

  const auto point = clamp_to_layout({640.5, 360.25}, bounds);
  EXPECT_DOUBLE_EQ(640.5, point.x);
  EXPECT_DOUBLE_EQ(360.25, point.y);
}

TEST(MacosMouseUtils, RepeatedDeltaAccumulatesExactlyOnce) {
  const bounds_t bounds {0, 0, 1920, 1080};
  CGPoint point {100, 100};
  for (int i = 0; i < 100; ++i) {
    point = clamp_to_layout({point.x + 3, point.y + 2}, bounds);
  }

  EXPECT_DOUBLE_EQ(400.0, point.x);
  EXPECT_DOUBLE_EQ(300.0, point.y);
}

TEST(MacosMouseUtils, AlternatingDeltaNetsToZero) {
  const bounds_t bounds {0, 0, 1920, 1080};
  CGPoint point {900, 500};
  for (int i = 0; i < 200; ++i) {
    const double delta = (i % 2 == 0) ? 7 : -7;
    point = clamp_to_layout({point.x + delta, point.y}, bounds);
  }

  EXPECT_DOUBLE_EQ(900.0, point.x);
  EXPECT_DOUBLE_EQ(500.0, point.y);
}

TEST(MacosMouseUtils, MoveTypeFollowsTheHeldButton) {
  EXPECT_EQ(kCGEventMouseMoved, move_event_type(false, false, false));
  EXPECT_EQ(kCGEventLeftMouseDragged, move_event_type(true, false, false));
  EXPECT_EQ(kCGEventRightMouseDragged, move_event_type(false, true, false));
  EXPECT_EQ(kCGEventOtherMouseDragged, move_event_type(false, false, true));

  // Only one drag type can describe a move, and macOS reports the left button first.
  EXPECT_EQ(kCGEventLeftMouseDragged, move_event_type(true, true, true));
}

TEST(MacosMouseUtils, MoveButtonMatchesTheMoveType) {
  EXPECT_EQ(kCGMouseButtonLeft, move_event_button(kCGEventMouseMoved));
  EXPECT_EQ(kCGMouseButtonLeft, move_event_button(kCGEventLeftMouseDragged));
  EXPECT_EQ(kCGMouseButtonRight, move_event_button(kCGEventRightMouseDragged));
  EXPECT_EQ(kCGMouseButtonCenter, move_event_button(kCGEventOtherMouseDragged));
}

#endif  // __APPLE__
