/**
 * @file tests/unit/test_mouse.cpp
 * @brief Test src/input.*.
 */
#include <src/input/common.h>
#include <src/platform/common.h>

#include <tests/conftest.cpp>

class MouseHIDTest: public virtual BaseTest, public PlatformInitBase, public ::testing::WithParamInterface<util::point_t> {
protected:
  void
  SetUp() override {
    BaseTest::SetUp();
    PlatformInitBase::SetUp();
#ifdef _WIN32
    // TODO: Windows tests are failing, `get_mouse_loc` seems broken and `platf::abs_mouse` too
    //       the alternative `platf::abs_mouse` method seem to work better during tests,
    //       but I'm not sure about real work
    GTEST_SKIP_("MouseTest:: skipped for now. TODO Windows");
#elif __linux__
    // TODO: Inputtino waiting https://github.com/games-on-whales/inputtino/issues/6 is resolved.
    GTEST_SKIP_("MouseTest:: skipped for now. TODO Inputtino");
#endif
  }

  void
  TearDown() override {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    PlatformInitBase::TearDown();
    BaseTest::TearDown();
  }
};
INSTANTIATE_TEST_SUITE_P(
  MouseInputs,
  MouseHIDTest,
  ::testing::Values(
    util::point_t { 40, 40 },
    util::point_t { 70, 150 }));
// todo: add tests for hitting screen edges

TEST_P(MouseHIDTest, MoveInputTest) {
  util::point_t mouse_delta = GetParam();

  std::cout << "MoveInputTest:: got param: " << mouse_delta << std::endl;
  platf::input_t input = platf::input();
  std::cout << "MoveInputTest:: init input" << std::endl;

  std::cout << "MoveInputTest:: get current mouse loc" << std::endl;
  auto old_loc = platf::get_mouse_loc(input);
  std::cout << "MoveInputTest:: got current mouse loc: " << old_loc << std::endl;

  std::cout << "MoveInputTest:: move: " << mouse_delta << std::endl;
  platf::move_mouse(input, mouse_delta.x, mouse_delta.y);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::cout << "MoveInputTest:: moved: " << mouse_delta << std::endl;

  std::cout << "MoveInputTest:: get updated mouse loc" << std::endl;
  auto new_loc = platf::get_mouse_loc(input);
  std::cout << "MoveInputTest:: got updated mouse loc: " << new_loc << std::endl;

  bool has_input_moved = old_loc.x != new_loc.x && old_loc.y != new_loc.y;

  if (!has_input_moved) {
    std::cout << "MoveInputTest:: haven't moved" << std::endl;
  }
  else {
    std::cout << "MoveInputTest:: moved" << std::endl;
  }

  EXPECT_TRUE(has_input_moved);

  // Verify we moved as much as we requested
  EXPECT_EQ(new_loc.x - old_loc.x, mouse_delta.x);
  EXPECT_EQ(new_loc.y - old_loc.y, mouse_delta.y);
}

TEST_P(MouseHIDTest, AbsMoveInputTest) {
  util::point_t mouse_pos = GetParam();
  std::cout << "AbsMoveInputTest:: got param: " << mouse_pos << std::endl;

  platf::input_t input = platf::input();
  std::cout << "AbsMoveInputTest:: init input" << std::endl;

  std::cout << "AbsMoveInputTest:: get current mouse loc" << std::endl;
  auto old_loc = platf::get_mouse_loc(input);
  std::cout << "AbsMoveInputTest:: got current mouse loc: " << old_loc << std::endl;

#ifdef _WIN32
  platf::touch_port_t abs_port {
    0, 0,
    65535, 65535
  };
#elif __linux__
  platf::touch_port_t abs_port {
    0, 0,
    19200, 12000
  };
#else
  platf::touch_port_t abs_port {};
#endif
  std::cout << "AbsMoveInputTest:: move: " << mouse_pos << std::endl;
  platf::abs_mouse(input, abs_port, mouse_pos.x, mouse_pos.y);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::cout << "AbsMoveInputTest:: moved: " << mouse_pos << std::endl;

  std::cout << "AbsMoveInputTest:: get updated mouse loc" << std::endl;
  auto new_loc = platf::get_mouse_loc(input);
  std::cout << "AbsMoveInputTest:: got updated mouse loc: " << new_loc << std::endl;

  bool has_input_moved = old_loc.x != new_loc.x || old_loc.y != new_loc.y;

  if (!has_input_moved) {
    std::cout << "AbsMoveInputTest:: haven't moved" << std::endl;
  }
  else {
    std::cout << "AbsMoveInputTest:: moved" << std::endl;
  }

  EXPECT_TRUE(has_input_moved);

  // Verify we moved to the absolute coordinate
  EXPECT_EQ(new_loc.x, mouse_pos.x);
  EXPECT_EQ(new_loc.y, mouse_pos.y);
}
