/**
 * @file tests/unit/test_text_context.cpp
 * @brief Regression tests for negotiated remote focus, gesture ownership and v1 geometry.
 */
// test includes
#include "../tests_common.h"

// local includes
#include "src/text_context.h"

namespace {
  using namespace text_context;
  using namespace std::chrono_literals;
  constexpr rect_t capture {-2560, -200, 0, 1240};
  constexpr editor_t editor {{-2510, -150, -2300, -80}, rect_t {-2490, -140, -2489, -120}, true};
  constexpr auto started = text_context::clock_t::time_point {};

  /** @brief Complete one click near the top left of the negative-origin monitor. */
  void click(router_t &router, session_id id, std::uint64_t pointer = mouse_pointer) {
    router.pointer(id, pointer, action_e::down, -2500, -130, capture, started);
    router.pointer(id, pointer, action_e::up, -2500, -130, capture, started);
  }

  TEST(RemoteTextContext, EncodesPhysicalCaptureCoordinatesAndPasswordMetadata) {
    router_t router;
    router.subscribe(1);
    click(router, 1);
    router.observe(router.generation(), editor, started);
    auto out = router.drain(1);
    ASSERT_EQ(out.size(), 1u);
    const packet_t golden {
      1,
      76,
      0xf7,
      0,
      1,
      0,
      0,
      0,
      1,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      1,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      2,
      2,
      0,
      0,
      60,
      0,
      0,
      0,
      70,
      0,
      0,
      0,
      50,
      0,
      0,
      0,
      50,
      0,
      0,
      0,
      4,
      1,
      0,
      0,
      120,
      0,
      0,
      0,
      70,
      0,
      0,
      0,
      60,
      0,
      0,
      0,
      71,
      0,
      0,
      0,
      80,
      0,
      0,
      0,
      0,
      10,
      0,
      0,
      160,
      5,
      0,
      0
    };
    EXPECT_EQ(out.front(), golden);
  }

  TEST(RemoteTextContext, RejectsDragsCancelledGesturesAndExpiredSnapshots) {
    router_t router;
    router.subscribe(1);
    for (auto action : {action_e::move, action_e::cancel, action_e::cancel_touches}) {
      router.pointer(1, 7, action_e::down, -2500, -130, capture, started);
      router.observe(router.generation(), editor, started);
      router.pointer(1, 7, action, -2400, -130, capture, started);
      router.pointer(1, 7, action_e::up, -2500, -130, capture, started);
      router.observe(router.generation(), editor, started);
      EXPECT_TRUE(router.drain(1).empty());
    }
    click(router, 1, 7);
    router.observe(router.generation(), editor, started + 1201ms);
    EXPECT_TRUE(router.drain(1).empty());
    EXPECT_FALSE(router.interested(started + 1201ms));
  }

  TEST(RemoteTextContext, AmbiguousHitsAndDisconnectedGenerationsCannotActivate) {
    router_t router;
    router.subscribe(1);
    router.subscribe(2);
    click(router, 1);
    auto old = router.generation();
    click(router, 2);
    router.observe(old, editor, started);
    router.observe(router.generation(), editor, started);
    EXPECT_TRUE(router.drain(1).empty());
    EXPECT_TRUE(router.drain(2).empty());
    router.unsubscribe(2);
    router.observe(router.generation(), editor, started);
    EXPECT_TRUE(router.drain(1).empty());
    click(router, 1);
    old = router.generation();
    router.unsubscribe(1);
    router.subscribe(3);
    click(router, 3);
    router.observe(old, editor, started);
    EXPECT_TRUE(router.drain(3).empty());
  }

  TEST(RemoteTextContext, MultiFingerGestureDoesNotRequestAKeyboard) {
    router_t router;
    router.subscribe(1);
    for (const auto action : {action_e::down, action_e::up}) {
      for (const auto pointer : {0, 1}) {
        router.pointer(1, pointer, action, -2500 + pointer, -130, capture, started);
      }
    }
    router.observe(router.generation(), editor, started);
    EXPECT_TRUE(router.drain(1).empty());
    click(router, 1, 0);
    router.observe(router.generation(), editor, started);
    EXPECT_EQ(router.drain(1).size(), 1u);
  }

  TEST(RemoteTextContext, FocusTransferAndLocalBlurCloseOnlyTheirOwners) {
    router_t router;
    router.subscribe(1);
    router.subscribe(2);
    click(router, 1);
    router.observe(router.generation(), editor, started);
    const auto first = router.drain(1);
    ASSERT_EQ(first.size(), 1u);
    click(router, 2, 0);
    router.observe(router.generation(), editor, started);
    const auto close = router.drain(1);
    const auto open = router.drain(2);
    ASSERT_EQ(close.size(), 1u);
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(close.front()[2], 0x90);
    EXPECT_EQ(close.front()[8], first.front()[8]);
    EXPECT_EQ(open.front()[25], 1);
    EXPECT_EQ(open.front()[4], 1);
    router.observe(router.generation(), std::nullopt, started);
    ASSERT_EQ(router.drain(2).size(), 1u);
    EXPECT_FALSE(router.interested(started));
  }

  TEST(RemoteTextContext, MissesCanBeRetriedUntilTheProviderReceivesFocus) {
    router_t router;
    router.subscribe(1);
    click(router, 1);
    router.observe(router.generation(), std::nullopt, started);
    const editor_t miss {{10, 20, 300, 100}, {}, false};
    router.observe(router.generation(), miss, started);
    EXPECT_TRUE(router.drain(1).empty());
    router.observe(router.generation(), editor, started + 100ms);
    ASSERT_EQ(router.drain(1).size(), 1u);
  }
}  // namespace
