/**
 * @file tests/unit/platform/test_virtualhid_input.cpp
 * @brief Tests for shared libvirtualhid input helpers.
 */

// test includes
#include "../../tests_common.h"
#include "../../tests_log_checker.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// lib includes
#include <lizardbyte/common/env.h>

#if defined(_WIN32) || defined(__APPLE__)
  #include <libvirtualhid/license.hpp>
#endif

// local includes
#include "src/config.h"
#include "src/globals.h"
#include "src/platform/virtualhid_input.h"

using namespace std::chrono_literals;
using namespace std::literals;

namespace {

  /**
   * @brief Expected touchpad support for a configured gamepad.
   */
  struct gamepad_capabilities_case_t {
    std::string_view gamepad;  ///< Configured gamepad name.
    bool expected_touchpad;  ///< Whether the configured gamepad supports touchpad input.
    bool expected_controller_extensions;  ///< Whether Moonlight controller extensions should be advertised.
  };

  /**
   * @brief Parameterized fixture that restores the configured gamepad after each test.
   */
  class VirtualHidInputTest: public ::testing::TestWithParam<gamepad_capabilities_case_t> {
  protected:
    /**
     * @brief Preserve the configured gamepad.
     */
    void SetUp() override {
      original_gamepad = config::input.gamepad;
    }

    /**
     * @brief Restore the configured gamepad.
     */
    void TearDown() override {
      config::input.gamepad = std::move(original_gamepad);
    }

  private:
    std::string original_gamepad;  ///< Configured gamepad restored after each test.
  };

}  // namespace

TEST_P(VirtualHidInputTest, ReportsExpectedTouchpadSupport) {
  const auto &test_case = GetParam();
  config::input.gamepad = test_case.gamepad;
  EXPECT_EQ(platf::virtualhid::configured_gamepad_supports_touchpad(), test_case.expected_touchpad) << test_case.gamepad;
}

TEST_P(VirtualHidInputTest, ReportsExpectedControllerExtensionSupport) {
  const auto &test_case = GetParam();
  config::input.gamepad = test_case.gamepad;
  EXPECT_EQ(
    platf::virtualhid::configured_gamepad_supports_controller_extensions(),
    test_case.expected_controller_extensions
  ) << test_case.gamepad;
}

INSTANTIATE_TEST_SUITE_P(
  ConfiguredGamepads,
  VirtualHidInputTest,
  ::testing::Values(
    gamepad_capabilities_case_t {"auto"sv, true, true},
    gamepad_capabilities_case_t {"generic"sv, false, false},
    gamepad_capabilities_case_t {"x360"sv, false, false},
    gamepad_capabilities_case_t {"xone"sv, false, false},
    gamepad_capabilities_case_t {"xseries"sv, false, false},
    gamepad_capabilities_case_t {"ds4"sv, true, true},
    gamepad_capabilities_case_t {"ds5"sv, true, true},
    gamepad_capabilities_case_t {"switch"sv, false, true}
  ),
  [](const ::testing::TestParamInfo<gamepad_capabilities_case_t> &info) {
    return std::string {info.param.gamepad};
  }
);

namespace {

  /**
   * @brief Fixture backed by libvirtualhid's observable in-memory devices.
   */
  class VirtualHidDeviceTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve input configuration and create fake virtual devices.
     */
    void SetUp() override {
      original_input = config::input;
      context_ = std::make_unique<platf::virtualhid::input_context_t>(lvh::BackendKind::fake);
      client_ = std::make_unique<platf::virtualhid::client_context_t>(*context_);
      const auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
      const std::string queue_name = std::string {"virtualhid-input-test-"} + test_info->test_suite_name() + '-' + test_info->name();
      feedback_queue_ = mail::man->queue<platf::gamepad_feedback_msg_t>(queue_name);

      ASSERT_NE(context_->runtime, nullptr);
      ASSERT_NE(context_->keyboard, nullptr);
      ASSERT_NE(context_->mouse, nullptr);
      ASSERT_NE(client_->touch, nullptr);
      ASSERT_NE(client_->pen, nullptr);
    }

    /**
     * @brief Restore process-wide input configuration.
     */
    void TearDown() override {
      client_.reset();
      context_.reset();
      feedback_queue_.reset();
      config::input = std::move(original_input);
    }

    /**
     * @brief Allocate a fake gamepad and return its state adapter.
     *
     * @param profile Configured Sunshine profile name.
     * @param type Client-reported controller type.
     * @param capabilities Client-reported capability flags.
     * @param global_index Global Sunshine gamepad slot.
     * @param client_index Client-relative gamepad index.
     * @return Allocated adapter, or `nullptr` when allocation fails.
     */
    lvh::GamepadStateAdapter *allocate_gamepad(
      std::string_view profile,
      std::uint8_t type = LI_CTYPE_UNKNOWN,
      std::uint16_t capabilities = 0,
      int global_index = 0,
      std::uint8_t client_index = 3
    ) {
      config::input.gamepad = profile;
      const platf::gamepad_id_t id {global_index, client_index};
      const platf::gamepad_arrival_t metadata {type, capabilities, 0};
      if (platf::virtualhid::alloc_gamepad(*context_, id, metadata, feedback_queue_) != 0) {
        ADD_FAILURE() << "Failed to allocate fake gamepad profile " << profile;
        return nullptr;
      }

      return platf::virtualhid::gamepad_adapter_for_testing(*context_, global_index);
    }

    /**
     * @brief Access the fake global virtual HID context.
     *
     * @return Pointer to the fixture-owned global context.
     */
    platf::virtualhid::input_context_t *context() const {
      return context_.get();
    }

    /**
     * @brief Access the fake per-client virtual HID context.
     *
     * @return Pointer to the fixture-owned client context.
     */
    platf::virtualhid::client_context_t *client() const {
      return client_.get();
    }

    /**
     * @brief Access the gamepad feedback queue.
     *
     * @return Reference to the fixture-owned feedback queue.
     */
    platf::feedback_queue_t &feedback_queue() {
      return feedback_queue_;
    }

  private:
    config::input_t original_input;  ///< Input configuration restored after each test.
    std::unique_ptr<platf::virtualhid::input_context_t> context_;  ///< Shared fake virtual devices.
    std::unique_ptr<platf::virtualhid::client_context_t> client_;  ///< Per-client fake touch and pen devices.
    platf::feedback_queue_t feedback_queue_;  ///< Captured gamepad feedback messages.
  };

  /**
   * @brief Auto-selection scenario for a client-reported gamepad.
   */
  struct auto_profile_case_t {
    const char *name;  ///< Stable parameter name.
    std::uint8_t type;  ///< Client-reported controller type.
    std::uint16_t capabilities;  ///< Client-reported capability flags.
    lvh::GamepadProfileKind expected;  ///< Expected libvirtualhid profile.
  };

  /**
   * @brief Parameterized fixture for automatic gamepad profile selection.
   */
  class VirtualHidAutoProfileTest:
      public VirtualHidDeviceTest,
      public ::testing::WithParamInterface<auto_profile_case_t> {};

}  // namespace

TEST(VirtualHidSeatTest, LeavesDeviceNameUnchangedForDefaultSeat) {
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("libvirtualhid Keyboard"sv, ""sv), "libvirtualhid Keyboard");
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("libvirtualhid Keyboard"sv, "seat0"sv), "libvirtualhid Keyboard");
}

TEST(VirtualHidSeatTest, AppendsNonDefaultSeatToDeviceName) {
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("libvirtualhid Mouse"sv, "seat1"sv), "libvirtualhid Mouse (seat1)");
  EXPECT_EQ(platf::virtualhid::device_name_for_seat("Sunshine DualSense"sv, "seat_vr"sv), "Sunshine DualSense (seat_vr)");
}

namespace {

  /**
   * @brief Fixture that creates fake virtual devices while `XDG_SEAT` targets a secondary seat.
   */
  class VirtualHidSeatDeviceTest: public ::testing::Test {
  protected:
    /**
     * @brief Point `XDG_SEAT` at `seat1` before creating fake devices.
     */
    void SetUp() override {
      if (std::string value; lizardbyte::common::get_env("XDG_SEAT", value)) {
        original_seat_ = std::move(value);
      }
      original_input_ = config::input;
      ASSERT_EQ(lizardbyte::common::set_env("XDG_SEAT", "seat1"), 0);
      context_ = std::make_unique<platf::virtualhid::input_context_t>(lvh::BackendKind::fake);
      client_ = std::make_unique<platf::virtualhid::client_context_t>(*context_);
      feedback_queue_ = mail::man->queue<platf::gamepad_feedback_msg_t>("virtualhid-input-test-seat");
    }

    /**
     * @brief Release fake devices and restore the original seat and input configuration.
     */
    void TearDown() override {
      client_.reset();
      context_.reset();
      feedback_queue_.reset();
      config::input = std::move(original_input_);
      if (original_seat_) {
        lizardbyte::common::set_env("XDG_SEAT", *original_seat_);
      } else {
        lizardbyte::common::unset_env("XDG_SEAT");
      }
    }

    std::unique_ptr<platf::virtualhid::input_context_t> context_;  ///< Shared fake virtual devices.
    std::unique_ptr<platf::virtualhid::client_context_t> client_;  ///< Per-client fake touch and pen devices.
    platf::feedback_queue_t feedback_queue_;  ///< Captured gamepad feedback messages.

  private:
    std::optional<std::string> original_seat_;  ///< `XDG_SEAT` value restored after each test.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };

}  // namespace

TEST_F(VirtualHidSeatDeviceTest, ScopesEveryDeviceNameToTargetSeat) {
  ASSERT_NE(context_->keyboard, nullptr);
  ASSERT_NE(context_->mouse, nullptr);
  ASSERT_NE(client_->touch, nullptr);
  ASSERT_NE(client_->pen, nullptr);
  EXPECT_EQ(context_->keyboard->profile().name, lvh::profiles::keyboard().name + " (seat1)");
  EXPECT_EQ(context_->mouse->profile().name, lvh::profiles::mouse().name + " (seat1)");
  EXPECT_EQ(client_->touch->profile().name, lvh::profiles::touchscreen().name + " (seat1)");
  EXPECT_EQ(client_->pen->profile().name, lvh::profiles::pen_tablet().name + " (seat1)");

  config::input.gamepad = "x360";
  const platf::gamepad_id_t id {0, 0};
  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  ASSERT_EQ(platf::virtualhid::alloc_gamepad(*context_, id, metadata, feedback_queue_), 0);
  auto *adapter = platf::virtualhid::gamepad_adapter_for_testing(*context_, 0);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(adapter->gamepad(), nullptr);
  EXPECT_TRUE(adapter->gamepad()->profile().name.starts_with("Sunshine "));
  EXPECT_TRUE(adapter->gamepad()->profile().name.ends_with(" (seat1)"));
  platf::virtualhid::free_gamepad(*context_, 0);
}

TEST_F(VirtualHidDeviceTest, CreatesEveryDeviceWithFakeRuntime) {
  EXPECT_EQ(context()->runtime->backend_kind(), lvh::BackendKind::fake);
  EXPECT_EQ(context()->runtime->capabilities().backend_name, "fake");
  EXPECT_TRUE(context()->runtime->capabilities().supports_gamepad);
  EXPECT_TRUE(context()->keyboard->is_open());
  EXPECT_TRUE(context()->mouse->is_open());
  EXPECT_TRUE(client()->touch->is_open());
  EXPECT_TRUE(client()->pen->is_open());

  const auto runtime = platf::virtualhid::create_runtime(lvh::BackendKind::fake);
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(runtime->backend_kind(), lvh::BackendKind::fake);
}

TEST_F(VirtualHidDeviceTest, SelectsVirtualHidGamepadRuntimeByBackendLicenseAndPreference) {
  auto capabilities = context()->runtime->capabilities();
  capabilities.supports_gamepad = true;
  capabilities.requires_installed_driver = false;
  EXPECT_TRUE(platf::virtualhid::should_use_gamepad_runtime(capabilities, "", false));
  EXPECT_TRUE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_ALL, false));

  capabilities.requires_installed_driver = true;
  EXPECT_FALSE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_ALL, false));
  EXPECT_TRUE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_ALL, true));
  EXPECT_TRUE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_VIRTUALHID, true));
  EXPECT_FALSE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_VIGEMBUS, true));
  EXPECT_FALSE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_NONE, true));

  capabilities.supports_gamepad = false;
  EXPECT_FALSE(platf::virtualhid::should_use_gamepad_runtime(capabilities, config::GAMEPAD_DRIVER_ALL, true));
}

TEST_F(VirtualHidDeviceTest, SelectsVigembusFallbackByBackendAndConfiguredProfile) {
  EXPECT_TRUE(platf::virtualhid::should_try_vigembus_fallback("auto", true, config::GAMEPAD_DRIVER_ALL));
  EXPECT_TRUE(platf::virtualhid::should_try_vigembus_fallback("x360", true, config::GAMEPAD_DRIVER_ALL));
  EXPECT_TRUE(platf::virtualhid::should_try_vigembus_fallback("ds4", true, config::GAMEPAD_DRIVER_ALL));
  EXPECT_FALSE(platf::virtualhid::should_try_vigembus_fallback("xseries", true, config::GAMEPAD_DRIVER_ALL));

  EXPECT_TRUE(platf::virtualhid::should_try_vigembus_fallback("xseries", false, config::GAMEPAD_DRIVER_ALL));
  EXPECT_TRUE(platf::virtualhid::should_try_vigembus_fallback("xseries", false, ""));
  EXPECT_TRUE(platf::virtualhid::should_try_vigembus_fallback("xseries", true, config::GAMEPAD_DRIVER_VIGEMBUS));
  EXPECT_FALSE(platf::virtualhid::should_try_vigembus_fallback("auto", false, config::GAMEPAD_DRIVER_VIRTUALHID));
  EXPECT_FALSE(platf::virtualhid::should_try_vigembus_fallback("auto", false, config::GAMEPAD_DRIVER_NONE));
}

TEST_F(VirtualHidDeviceTest, ReportsStaticAndRuntimeGamepadChoices) {
  constexpr std::array expected_names {"auto"sv, "generic"sv, "x360"sv, "xone"sv, "xseries"sv, "ds4"sv, "ds5"sv, "switch"sv};

  const auto static_gamepads = platf::virtualhid::static_supported_gamepads();
  ASSERT_EQ(static_gamepads.size(), expected_names.size());
  EXPECT_TRUE(static_gamepads.front().is_enabled);
  for (std::size_t index = 0; index < expected_names.size(); ++index) {
    EXPECT_EQ(static_gamepads[index].name, expected_names[index]);
    EXPECT_TRUE(static_gamepads[index].reason_disabled.empty());
  }

  const auto available_gamepads = platf::virtualhid::supported_gamepads(context()->runtime.get());
  ASSERT_EQ(available_gamepads.size(), expected_names.size());
  for (const auto &gamepad : available_gamepads) {
    EXPECT_TRUE(gamepad.is_enabled) << gamepad.name;
    EXPECT_TRUE(gamepad.reason_disabled.empty()) << gamepad.name;
  }

  const auto licensed_broker_gamepads = platf::virtualhid::supported_gamepads(context()->runtime.get(), false, true, true);
  for (const auto &gamepad : licensed_broker_gamepads) {
    EXPECT_TRUE(gamepad.is_enabled) << gamepad.name;
  }

  const auto unlicensed_gamepads = platf::virtualhid::supported_gamepads(context()->runtime.get(), false, false, true);
  ASSERT_EQ(unlicensed_gamepads.size(), expected_names.size());
  for (const auto &gamepad : unlicensed_gamepads) {
    EXPECT_FALSE(gamepad.is_enabled) << gamepad.name;
    EXPECT_EQ(gamepad.reason_disabled, "gamepads.virtualhid-license-invalid") << gamepad.name;
  }

  const auto no_runtime_gamepads = platf::virtualhid::supported_gamepads(nullptr, true);
  EXPECT_EQ(no_runtime_gamepads.size(), expected_names.size());
}

#if defined(_WIN32) || defined(__APPLE__)
TEST_F(VirtualHidDeviceTest, NoneSelectionHidesAvailableGamepads) {
  config::input.gamepad_driver = config::GAMEPAD_DRIVER_NONE;
  auto platform_input = platf::input();
  ASSERT_TRUE(platform_input);

  EXPECT_TRUE(platf::supported_gamepads(std::addressof(platform_input)).empty());
  EXPECT_EQ(platf::get_capabilities() & platf::platform_caps::controller_touch, 0U);
}

TEST_F(VirtualHidDeviceTest, BrokerCreatesAndUpdatesEveryGamepadProfileWhenLicensed) {
  if (!lvh::get_license_status().license.licensed()) {
    GTEST_SKIP() << "Virtual HID Broker is not activated";
  }

  config::input.gamepad_driver = config::GAMEPAD_DRIVER_VIRTUALHID;
  auto platform_input = platf::input();
  ASSERT_TRUE(platform_input);
  const auto &context = platf::virtualhid::get_input_context(platform_input);
  if (!context.runtime || !context.runtime->capabilities().supports_gamepad) {
    GTEST_SKIP() << "Virtual HID Broker is not installed";
  }

  const auto &available = platf::supported_gamepads(std::addressof(platform_input));
  for (const auto &gamepad : available) {
    EXPECT_TRUE(gamepad.is_enabled) << gamepad.name;
  }

  constexpr std::array profiles {"generic"sv, "x360"sv, "xone"sv, "xseries"sv, "ds4"sv, "ds5"sv, "switch"sv};
  const platf::gamepad_id_t id {0, 0};
  const platf::gamepad_arrival_t metadata {LI_CTYPE_UNKNOWN, 0, 0};
  for (const auto profile : profiles) {
    config::input.gamepad = profile;
    ASSERT_EQ(platf::alloc_gamepad(platform_input, id, metadata, nullptr), 0) << profile;
    platf::gamepad_update(platform_input, 0, {platf::A, 255, 0, 0, 0, 0, 0});
    const auto *adapter = platf::virtualhid::gamepad_adapter_for_testing(platf::virtualhid::get_input_context(platform_input), 0);
    ASSERT_NE(adapter, nullptr);
    EXPECT_TRUE(adapter->state().buttons.test(lvh::GamepadButton::a)) << profile;
    platf::free_gamepad(platform_input, 0);
  }
}
#endif

TEST_F(VirtualHidDeviceTest, RejectsUnavailableAndInvalidGamepadSlots) {
  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  const platf::gamepad_id_t valid_id {0, 0};

  platf::virtualhid::input_context_t no_runtime {lvh::BackendKind::fake};
  no_runtime.runtime.reset();
  no_runtime.refresh_keyboard();
  no_runtime.refresh_mouse();
  EXPECT_EQ(no_runtime.keyboard, nullptr);
  EXPECT_EQ(no_runtime.mouse, nullptr);
  EXPECT_EQ(platf::virtualhid::alloc_gamepad(no_runtime, valid_id, metadata, nullptr), -1);

  EXPECT_EQ(platf::virtualhid::alloc_gamepad(*context(), {-1, 0}, metadata, nullptr), -1);
  EXPECT_EQ(platf::virtualhid::alloc_gamepad(*context(), {static_cast<int>(context()->gamepads.size()), 0}, metadata, nullptr), -1);
  EXPECT_FALSE(platf::virtualhid::has_gamepad(*context(), -1));
  EXPECT_FALSE(platf::virtualhid::has_gamepad(*context(), static_cast<int>(context()->gamepads.size())));
  EXPECT_EQ(platf::virtualhid::gamepad_adapter_for_testing(*context(), 0), nullptr);
  EXPECT_EQ(platf::virtualhid::rebind_gamepad(*context(), valid_id, feedback_queue()), -1);
}

TEST_F(VirtualHidDeviceTest, RoutesLibvirtualhidDiagnosticsToSunshineLog) {
  auto runtime = platf::virtualhid::create_runtime(lvh::BackendKind::fake);
  ASSERT_NE(runtime, nullptr);

  auto created = runtime->create_mouse();
  ASSERT_TRUE(created);
  ASSERT_NE(created.mouse, nullptr);
  EXPECT_TRUE(created.mouse->move_relative(1, -1).ok());
  EXPECT_FALSE(created.mouse->move_absolute(0, 0, 0, 1).ok());
  EXPECT_TRUE(created.mouse->close().ok());
  EXPECT_FALSE(created.mouse->move_relative(1, 1).ok());

  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Info: [libvirtualhid] initialized fake backend"));
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Debug: [libvirtualhid] mouse "));
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Warning: [libvirtualhid] rejected mouse event:"));
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Error: [libvirtualhid] mouse input failed: mouse is closed"));
}

TEST_F(VirtualHidDeviceTest, AllocatesManualProfileAndTranslatesFullState) {
  const auto active_devices_before_gamepad = context()->runtime->active_device_count();
  config::input.ds4_back_as_touchpad_click = true;
  config::input.virtualhid_randomize_mac = false;
  const auto capabilities = static_cast<std::uint16_t>(LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_RGB_LED | LI_CCAP_BATTERY_STATE);
  auto *adapter = allocate_gamepad("ds5"sv, LI_CTYPE_PS, capabilities, 1, 4);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(adapter->gamepad(), nullptr);
  EXPECT_EQ(context()->runtime->active_device_count(), active_devices_before_gamepad + 1);

  EXPECT_EQ(adapter->gamepad()->profile().gamepad_kind, lvh::GamepadProfileKind::dualsense);
  EXPECT_TRUE(adapter->gamepad()->profile().name.starts_with("Sunshine "));
  const auto &metadata = adapter->gamepad()->metadata();
  EXPECT_EQ(metadata.global_index, 1);
  EXPECT_EQ(metadata.client_relative_index, 4);
  EXPECT_EQ(metadata.client_type, lvh::ClientControllerType::playstation);
  EXPECT_TRUE(metadata.has_motion_sensors);
  EXPECT_TRUE(metadata.has_touchpad);
  EXPECT_TRUE(metadata.has_rgb_led);
  EXPECT_TRUE(metadata.has_battery);
  EXPECT_EQ(metadata.stable_id, "02:00:00:00:00:01");

  const std::uint32_t button_flags = platf::DPAD_UP | platf::DPAD_DOWN | platf::DPAD_LEFT | platf::DPAD_RIGHT | platf::START | platf::BACK |
                                     platf::LEFT_STICK | platf::RIGHT_STICK | platf::LEFT_BUTTON | platf::RIGHT_BUTTON | platf::HOME |
                                     platf::A | platf::B | platf::X | platf::Y | platf::MISC_BUTTON;
  const platf::gamepad_state_t input_state {
    button_flags,
    128,
    255,
    std::numeric_limits<std::int16_t>::min(),
    std::numeric_limits<std::int16_t>::max(),
    -16384,
    0,
  };
  platf::virtualhid::gamepad_update(*context(), 1, input_state);

  const auto &state = adapter->state();
  using enum lvh::GamepadButton;
  for (const auto button : {dpad_up, dpad_down, dpad_left, dpad_right, start, back, left_stick, right_stick, left_shoulder, right_shoulder, guide, a, b, x, y, misc1, touchpad}) {
    EXPECT_TRUE(state.buttons.test(button));
  }
  EXPECT_FLOAT_EQ(state.left_stick.x, -1.0F);
  EXPECT_FLOAT_EQ(state.left_stick.y, 1.0F);
  EXPECT_FLOAT_EQ(state.right_stick.x, -0.5F);
  EXPECT_FLOAT_EQ(state.right_stick.y, 0.0F);
  EXPECT_FLOAT_EQ(state.left_trigger, 128.0F / 255.0F);
  EXPECT_FLOAT_EQ(state.right_trigger, 1.0F);

  config::input.ds4_back_as_touchpad_click = false;
  platf::virtualhid::gamepad_update(*context(), 1, {platf::BACK, 0, 0, 0, 0, 0, 0});
  EXPECT_FALSE(adapter->state().buttons.test(touchpad));

  platf::virtualhid::free_gamepad(*context(), 1);
  EXPECT_FALSE(platf::virtualhid::has_gamepad(*context(), 1));
  EXPECT_EQ(context()->runtime->active_device_count(), active_devices_before_gamepad);
  platf::virtualhid::free_gamepad(*context(), 1);
  platf::virtualhid::gamepad_update(*context(), 1, input_state);
}

TEST_P(VirtualHidAutoProfileTest, SelectsProfileFromClientMetadata) {
  const auto &test_case = GetParam();
  auto *adapter = allocate_gamepad("auto"sv, test_case.type, test_case.capabilities);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(adapter->gamepad(), nullptr);
  EXPECT_EQ(adapter->gamepad()->profile().gamepad_kind, test_case.expected);
}

INSTANTIATE_TEST_SUITE_P(
  ClientMetadata,
  VirtualHidAutoProfileTest,
  ::testing::Values(
    auto_profile_case_t {"PlayStation", LI_CTYPE_PS, 0, lvh::GamepadProfileKind::dualsense},
    auto_profile_case_t {"Nintendo", LI_CTYPE_NINTENDO, 0, lvh::GamepadProfileKind::switch_pro},
    auto_profile_case_t {"Xbox", LI_CTYPE_XBOX, 0, lvh::GamepadProfileKind::xbox_series},
    auto_profile_case_t {"UnknownMotion", LI_CTYPE_UNKNOWN, LI_CCAP_ACCEL, lvh::GamepadProfileKind::dualsense},
    auto_profile_case_t {"UnknownTouchpad", LI_CTYPE_UNKNOWN, LI_CCAP_TOUCHPAD, lvh::GamepadProfileKind::dualsense},
    auto_profile_case_t {"UnknownDefault", LI_CTYPE_UNKNOWN, 0, lvh::GamepadProfileKind::xbox_series}
  ),
  [](const ::testing::TestParamInfo<auto_profile_case_t> &info) {
    return info.param.name;
  }
);

TEST_F(VirtualHidDeviceTest, FallsBackForUnknownManualProfileAndRandomizesPlayStationIdentity) {
  auto *fallback = allocate_gamepad("not-a-profile"sv, LI_CTYPE_UNKNOWN, 0, 0);
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ(fallback->gamepad()->profile().gamepad_kind, lvh::GamepadProfileKind::xbox_series);
  EXPECT_EQ(fallback->gamepad()->metadata().client_type, lvh::ClientControllerType::unknown);
  EXPECT_EQ(fallback->gamepad()->metadata().stable_id, "sunshine-gamepad-0");

  config::input.virtualhid_randomize_mac = true;
  auto *randomized = allocate_gamepad("ds4"sv, LI_CTYPE_PS, 0, 1);
  ASSERT_NE(randomized, nullptr);
  const auto &stable_id = randomized->gamepad()->metadata().stable_id;
  EXPECT_EQ(stable_id.size(), 17);
  EXPECT_TRUE(stable_id.starts_with("02:00:"));
}

TEST_F(VirtualHidDeviceTest, RoutesAndDeduplicatesGamepadFeedback) {
  auto *adapter = allocate_gamepad("xseries"sv, LI_CTYPE_XBOX);
  ASSERT_NE(adapter, nullptr);

  lvh::GamepadOutput output;
  output.kind = lvh::GamepadOutputKind::rumble;
  output.low_frequency_rumble = 100;
  output.high_frequency_rumble = 200;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  auto feedback = feedback_queue()->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->type, platf::gamepad_feedback_e::rumble);
  EXPECT_EQ(feedback->id, 3);
  EXPECT_EQ(feedback->data.rumble.lowfreq, 100);
  EXPECT_EQ(feedback->data.rumble.highfreq, 200);

  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_FALSE(feedback_queue()->pop(0ms));
  output.high_frequency_rumble = 201;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_TRUE(feedback_queue()->pop(10ms));

  output.kind = lvh::GamepadOutputKind::trigger_rumble;
  output.left_trigger_rumble = 300;
  output.right_trigger_rumble = 400;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  feedback = feedback_queue()->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->type, platf::gamepad_feedback_e::rumble_triggers);
  EXPECT_EQ(feedback->data.rumble_triggers.left_trigger, 300);
  EXPECT_EQ(feedback->data.rumble_triggers.right_trigger, 400);
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_FALSE(feedback_queue()->pop(0ms));
  ++output.right_trigger_rumble;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_TRUE(feedback_queue()->pop(10ms));

  output.kind = lvh::GamepadOutputKind::rgb_led;
  output.red = 10;
  output.green = 20;
  output.blue = 30;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  feedback = feedback_queue()->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->type, platf::gamepad_feedback_e::set_rgb_led);
  EXPECT_EQ(feedback->data.rgb_led.r, 10);
  EXPECT_EQ(feedback->data.rgb_led.g, 20);
  EXPECT_EQ(feedback->data.rgb_led.b, 30);
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_FALSE(feedback_queue()->pop(0ms));
  ++output.blue;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_TRUE(feedback_queue()->pop(10ms));

  output.kind = lvh::GamepadOutputKind::player_leds;
  output.player_leds = {true, false, true, false};
  output.flashing_player_leds = {false, true, false, true};
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  feedback = feedback_queue()->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->type, platf::gamepad_feedback_e::set_player_leds);
  EXPECT_EQ(feedback->data.player_leds.solid, 0x05);
  EXPECT_EQ(feedback->data.player_leds.flashing, 0x0A);
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_FALSE(feedback_queue()->pop(0ms));
  output.player_leds[3] = true;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  feedback = feedback_queue()->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->data.player_leds.solid, 0x0D);

  output.kind = lvh::GamepadOutputKind::adaptive_triggers;
  output.adaptive_trigger_flags = 5;
  output.left_trigger_effect_type = 6;
  output.right_trigger_effect_type = 7;
  output.left_trigger_effect = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  output.right_trigger_effect = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  feedback = feedback_queue()->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->type, platf::gamepad_feedback_e::set_adaptive_triggers);
  EXPECT_EQ(feedback->data.adaptive_triggers.event_flags, 5);
  EXPECT_EQ(feedback->data.adaptive_triggers.type_left, 6);
  EXPECT_EQ(feedback->data.adaptive_triggers.type_right, 7);
  EXPECT_EQ(feedback->data.adaptive_triggers.left, output.left_trigger_effect);
  EXPECT_EQ(feedback->data.adaptive_triggers.right, output.right_trigger_effect);

  auto resumed_feedback = mail::man->queue<platf::gamepad_feedback_msg_t>("virtualhid-input-test-resumed-feedback");
  ASSERT_EQ(platf::virtualhid::rebind_gamepad(*context(), {0, 7}, resumed_feedback), 0);
  output.kind = lvh::GamepadOutputKind::rumble;
  output.low_frequency_rumble = 100;
  output.high_frequency_rumble = 201;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  feedback = resumed_feedback->pop(10ms);
  ASSERT_TRUE(feedback);
  EXPECT_EQ(feedback->type, platf::gamepad_feedback_e::rumble);
  EXPECT_EQ(feedback->id, 7);
  EXPECT_FALSE(feedback_queue()->pop(0ms));

  output.kind = lvh::GamepadOutputKind::raw_report;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
  EXPECT_FALSE(feedback_queue()->pop(0ms));

  platf::virtualhid::free_gamepad(*context(), 0);
  const platf::gamepad_id_t id {0, 0};
  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  ASSERT_EQ(platf::virtualhid::alloc_gamepad(*context(), id, metadata, nullptr), 0);
  adapter = platf::virtualhid::gamepad_adapter_for_testing(*context(), 0);
  ASSERT_NE(adapter, nullptr);
  output.kind = lvh::GamepadOutputKind::rumble;
  ASSERT_TRUE(adapter->dispatch_output(output).ok());
}

TEST_F(VirtualHidDeviceTest, TranslatesGamepadTouchMotionAndBattery) {
  const auto capabilities = static_cast<std::uint16_t>(LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_BATTERY_STATE);
  auto *adapter = allocate_gamepad("ds5"sv, LI_CTYPE_PS, capabilities);
  ASSERT_NE(adapter, nullptr);
  EXPECT_TRUE(feedback_queue()->pop(10ms));
  EXPECT_TRUE(feedback_queue()->pop(10ms));

  platf::gamepad_touch_t touch {{0, 3}, LI_TOUCH_EVENT_DOWN, 10, -0.25F, 1.25F, 1.0F};
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_EQ(adapter->state().touchpad_contacts[0].id, 0);
  EXPECT_TRUE(adapter->state().touchpad_contacts[0].active);
  EXPECT_FLOAT_EQ(adapter->state().touchpad_contacts[0].x, 0.0F);
  EXPECT_FLOAT_EQ(adapter->state().touchpad_contacts[0].y, 1.0F);

  touch.eventType = LI_TOUCH_EVENT_MOVE;
  touch.pressure = 0.25F;
  touch.x = 0.4F;
  touch.y = 0.6F;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_FALSE(adapter->state().touchpad_contacts[0].active);
  EXPECT_FLOAT_EQ(adapter->state().touchpad_contacts[0].x, 0.4F);

  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 11;
  platf::virtualhid::gamepad_touch(*context(), touch);
  const auto full_touch_submit_count = adapter->gamepad()->submit_count();
  touch.pointerId = 12;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_EQ(adapter->gamepad()->submit_count(), full_touch_submit_count);

  touch.eventType = LI_TOUCH_EVENT_MOVE;
  touch.pointerId = 99;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_EQ(adapter->gamepad()->submit_count(), full_touch_submit_count);
  touch.eventType = 0xFF;
  touch.pointerId = 10;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_EQ(adapter->gamepad()->submit_count(), full_touch_submit_count);

  touch.eventType = LI_TOUCH_EVENT_CANCEL;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_FALSE(adapter->state().touchpad_contacts[0].active);
  touch.eventType = LI_TOUCH_EVENT_UP;
  touch.pointerId = 11;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_FALSE(adapter->state().touchpad_contacts[1].active);

  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 20;
  platf::virtualhid::gamepad_touch(*context(), touch);
  touch.pointerId = 21;
  platf::virtualhid::gamepad_touch(*context(), touch);
  touch.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  platf::virtualhid::gamepad_touch(*context(), touch);
  EXPECT_FALSE(adapter->state().touchpad_contacts[0].active);
  EXPECT_FALSE(adapter->state().touchpad_contacts[1].active);

  const platf::gamepad_motion_t acceleration {{0, 3}, LI_MOTION_TYPE_ACCEL, 1.0F, 2.0F, 3.0F};
  platf::virtualhid::gamepad_motion(*context(), acceleration);
  ASSERT_TRUE(adapter->state().acceleration);
  EXPECT_FLOAT_EQ(adapter->state().acceleration->x, 1.0F);
  const platf::gamepad_motion_t gyroscope {{0, 3}, LI_MOTION_TYPE_GYRO, 4.0F, 5.0F, 6.0F};
  platf::virtualhid::gamepad_motion(*context(), gyroscope);
  ASSERT_TRUE(adapter->state().gyroscope);
  EXPECT_FLOAT_EQ(adapter->state().gyroscope->z, 6.0F);
  const auto motion_submit_count = adapter->gamepad()->submit_count();
  platf::virtualhid::gamepad_motion(*context(), {{0, 3}, 0xFF, 0.0F, 0.0F, 0.0F});
  platf::virtualhid::gamepad_motion(*context(), {{7, 3}, LI_MOTION_TYPE_ACCEL, 0.0F, 0.0F, 0.0F});
  EXPECT_EQ(adapter->gamepad()->submit_count(), motion_submit_count);

  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_DISCHARGING, 75});
  ASSERT_TRUE(adapter->state().battery);
  EXPECT_EQ(adapter->state().battery->state, lvh::GamepadBatteryState::discharging);
  EXPECT_EQ(adapter->state().battery->percentage, 75);
  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_CHARGING, LI_BATTERY_PERCENTAGE_UNKNOWN});
  EXPECT_EQ(adapter->state().battery->state, lvh::GamepadBatteryState::charging);
  EXPECT_EQ(adapter->state().battery->percentage, 100);
  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_FULL, 120});
  EXPECT_EQ(adapter->state().battery->state, lvh::GamepadBatteryState::full);
  EXPECT_EQ(adapter->state().battery->percentage, 100);
  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_NOT_CHARGING, 50});
  EXPECT_EQ(adapter->state().battery->state, lvh::GamepadBatteryState::charging_error);
  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_UNKNOWN, 0});
  EXPECT_FALSE(adapter->state().battery);
  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_NOT_PRESENT, 0});
  EXPECT_FALSE(adapter->state().battery);
  platf::virtualhid::gamepad_battery(*context(), {{7, 3}, LI_BATTERY_STATE_FULL, 100});

  const auto unsupported_capabilities = static_cast<std::uint16_t>(LI_CCAP_ACCEL | LI_CCAP_TOUCHPAD | LI_CCAP_RGB_LED);
  auto *unsupported = allocate_gamepad("xseries"sv, LI_CTYPE_XBOX, unsupported_capabilities, 1);
  ASSERT_NE(unsupported, nullptr);
  const auto unsupported_count = unsupported->gamepad()->submit_count();
  platf::virtualhid::gamepad_touch(*context(), {{1, 3}, LI_TOUCH_EVENT_DOWN, 0, 0.5F, 0.5F, 1.0F});
  EXPECT_EQ(unsupported->gamepad()->submit_count(), unsupported_count);
}

TEST_F(VirtualHidDeviceTest, PreservesAuxiliaryGamepadStateAcrossControlUpdates) {
  const auto capabilities = static_cast<std::uint16_t>(LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_BATTERY_STATE);
  auto *adapter = allocate_gamepad("ds5"sv, LI_CTYPE_PS, capabilities);
  ASSERT_NE(adapter, nullptr);
  EXPECT_TRUE(feedback_queue()->pop(10ms));
  EXPECT_TRUE(feedback_queue()->pop(10ms));

  platf::virtualhid::gamepad_motion(*context(), {{0, 3}, LI_MOTION_TYPE_ACCEL, 0.0F, 9.80665F, 0.0F});
  platf::virtualhid::gamepad_motion(*context(), {{0, 3}, LI_MOTION_TYPE_GYRO, -0.2F, -0.5F, 0.0F});
  platf::virtualhid::gamepad_touch(*context(), {{0, 3}, LI_TOUCH_EVENT_DOWN, 10, 0.25F, 0.75F, 1.0F});
  platf::virtualhid::gamepad_battery(*context(), {{0, 3}, LI_BATTERY_STATE_DISCHARGING, 75});

  platf::virtualhid::gamepad_update(*context(), 0, {platf::A, 255, 0, 0, 0, 0, 0});

  const auto &state = adapter->state();
  EXPECT_TRUE(state.buttons.test(lvh::GamepadButton::a));
  ASSERT_TRUE(state.acceleration);
  EXPECT_FLOAT_EQ(state.acceleration->x, 0.0F);
  EXPECT_FLOAT_EQ(state.acceleration->y, 9.80665F);
  EXPECT_FLOAT_EQ(state.acceleration->z, 0.0F);
  ASSERT_TRUE(state.gyroscope);
  EXPECT_FLOAT_EQ(state.gyroscope->x, -0.2F);
  EXPECT_FLOAT_EQ(state.gyroscope->y, -0.5F);
  EXPECT_FLOAT_EQ(state.gyroscope->z, 0.0F);
  EXPECT_TRUE(state.touchpad_contacts[0].active);
  EXPECT_FLOAT_EQ(state.touchpad_contacts[0].x, 0.25F);
  EXPECT_FLOAT_EQ(state.touchpad_contacts[0].y, 0.75F);
  ASSERT_TRUE(state.battery);
  EXPECT_EQ(state.battery->state, lvh::GamepadBatteryState::discharging);
  EXPECT_EQ(state.battery->percentage, 75);
}

TEST_F(VirtualHidDeviceTest, TranslatesMouseAndKeyboardInput) {
  platf::virtualhid::move_mouse(*context(), -4, 7);
  auto mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, -4);
  EXPECT_EQ(mouse_event.y, 7);

  const platf::touch_port_t viewport {
    .offset_x = 10,
    .offset_y = 20,
    .width = 3840,
    .height = 2160,
    .logical_width = 1920,
    .logical_height = 1080,
  };
  platf::virtualhid::abs_mouse(*context(), viewport, 10.6F, 20.4F);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::absolute_motion);
  EXPECT_EQ(mouse_event.x, 1);
  EXPECT_EQ(mouse_event.y, 0);
  EXPECT_NEAR(mouse_event.absolute_x, 0.6F, 0.0001F);
  EXPECT_NEAR(mouse_event.absolute_y, 0.4F, 0.0001F);
  EXPECT_EQ(mouse_event.width, 1920);
  EXPECT_EQ(mouse_event.height, 1080);

  constexpr std::array button_cases {
    std::pair {BUTTON_LEFT, lvh::MouseButton::left},
    std::pair {BUTTON_MIDDLE, lvh::MouseButton::middle},
    std::pair {BUTTON_RIGHT, lvh::MouseButton::right},
    std::pair {BUTTON_X1, lvh::MouseButton::side},
    std::pair {BUTTON_X2, lvh::MouseButton::extra},
  };
  for (const auto &[button, expected] : button_cases) {
    platf::virtualhid::button_mouse(*context(), button, false);
    mouse_event = context()->mouse->last_submitted_event();
    EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::button);
    EXPECT_EQ(mouse_event.button, expected);
    EXPECT_TRUE(mouse_event.pressed);
    platf::virtualhid::button_mouse(*context(), button, true);
    EXPECT_FALSE(context()->mouse->last_submitted_event().pressed);
  }
  const auto button_submit_count = context()->mouse->submit_count();
  platf::virtualhid::button_mouse(*context(), 999, false);
  EXPECT_EQ(context()->mouse->submit_count(), button_submit_count);

  platf::virtualhid::scroll(*context(), 120);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::vertical_scroll);
  EXPECT_EQ(mouse_event.high_resolution_scroll, 120);
  platf::virtualhid::hscroll(*context(), -240);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::horizontal_scroll);
  EXPECT_EQ(mouse_event.high_resolution_scroll, -240);

  config::input.always_send_scancodes = true;
  platf::virtualhid::keyboard_update(*context(), 0x41, false, 0);
  auto keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_EQ(keyboard_event.key_code, 0x41);
  EXPECT_TRUE(keyboard_event.pressed);
  EXPECT_FALSE(keyboard_event.extended);
#ifdef _WIN32
  EXPECT_TRUE(keyboard_event.uses_normalized_key_code);
  EXPECT_TRUE(keyboard_event.prefer_native_scan_code);
  platf::virtualhid::keyboard_update(*context(), 0x41, true, SS_KBE_FLAG_NON_NORMALIZED);
  keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_FALSE(keyboard_event.uses_normalized_key_code);
#else
  EXPECT_TRUE(keyboard_event.uses_normalized_key_code);
  EXPECT_FALSE(keyboard_event.prefer_native_scan_code);
  platf::virtualhid::keyboard_update(*context(), 0x41, true, SS_KBE_FLAG_NON_NORMALIZED);
  keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_FALSE(keyboard_event.uses_normalized_key_code);
#endif
  EXPECT_FALSE(keyboard_event.pressed);
  platf::virtualhid::keyboard_update(*context(), 0x0D, false, 0, true);
  keyboard_event = context()->keyboard->last_submitted_event();
  EXPECT_EQ(keyboard_event.key_code, 0x0D);
  EXPECT_TRUE(keyboard_event.extended);

  const auto keyboard_submit_count = context()->keyboard->submit_count();
  const std::string text = "Sunshine \u{2600}";
  platf::virtualhid::unicode(*context(), text.data(), static_cast<int>(text.size()));
  EXPECT_EQ(context()->keyboard->submit_count(), keyboard_submit_count + 1);
  platf::virtualhid::unicode(*context(), nullptr, 1);
  platf::virtualhid::unicode(*context(), text.data(), 0);
  EXPECT_EQ(context()->keyboard->submit_count(), keyboard_submit_count + 1);

  ASSERT_TRUE(context()->mouse->close().ok());
  ASSERT_TRUE(context()->keyboard->close().ok());
  platf::virtualhid::move_mouse(*context(), 1, 1);
  platf::virtualhid::keyboard_update(*context(), 0x41, false, 0);
  context()->mouse.reset();
  context()->keyboard.reset();
  platf::virtualhid::move_mouse(*context(), 1, 1);
  platf::virtualhid::move_mouse(*context(), viewport, 1, 1);
  platf::virtualhid::abs_mouse(*context(), viewport, 1.0F, 1.0F);
  platf::virtualhid::button_mouse(*context(), BUTTON_LEFT, false);
  platf::virtualhid::scroll(*context(), 1);
  platf::virtualhid::hscroll(*context(), 1);
  platf::virtualhid::keyboard_update(*context(), 0x41, false, 0);
  platf::virtualhid::unicode(*context(), text.data(), static_cast<int>(text.size()));
}

TEST_F(VirtualHidDeviceTest, RetargetsAbsoluteMouseWhenStreamedViewportChanges) {
  const platf::touch_port_t left_viewport {
    .offset_x = -1920,
    .offset_y = 0,
    .width = 3840,
    .height = 1080,
    .logical_width = 1920,
    .logical_height = 1080,
    .env_offset_x = -1920,
    .env_offset_y = 0,
  };

  const auto initial_mouse_id = context()->mouse->device_id();
  platf::virtualhid::abs_mouse(*context(), left_viewport, -960.25F, 540.5F);
  const auto left_mouse_id = context()->mouse->device_id();
  EXPECT_NE(left_mouse_id, initial_mouse_id);
  auto mouse_event = context()->mouse->last_submitted_event();
  EXPECT_FLOAT_EQ(mouse_event.absolute_x, 959.75F);
  EXPECT_FLOAT_EQ(mouse_event.absolute_y, 540.5F);
  EXPECT_EQ(mouse_event.width, 1920);
  EXPECT_EQ(mouse_event.height, 1080);

  platf::virtualhid::abs_mouse(*context(), left_viewport, -480.0F, 270.0F);
  EXPECT_EQ(context()->mouse->device_id(), left_mouse_id);

  auto right_viewport = left_viewport;
  right_viewport.offset_x = 0;
  platf::virtualhid::abs_mouse(*context(), right_viewport, 960.0F, 540.0F);
  EXPECT_NE(context()->mouse->device_id(), left_mouse_id);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_FLOAT_EQ(mouse_event.absolute_x, 960.0F);
  EXPECT_FLOAT_EQ(mouse_event.absolute_y, 540.0F);
}

TEST_F(VirtualHidDeviceTest, RetargetsRelativeMouseWhenStreamedViewportChanges) {
  const platf::touch_port_t left_viewport {
    .offset_x = -1920,
    .offset_y = 0,
    .width = 3840,
    .height = 1080,
    .logical_width = 1920,
    .logical_height = 1080,
    .env_offset_x = -1920,
    .env_offset_y = 0,
  };

  const auto initial_mouse_id = context()->mouse->device_id();
  platf::virtualhid::move_mouse(*context(), left_viewport, 12, -7);
  const auto left_mouse_id = context()->mouse->device_id();
  EXPECT_NE(left_mouse_id, initial_mouse_id);
  auto mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, 12);
  EXPECT_EQ(mouse_event.y, -7);

  platf::virtualhid::move_mouse(*context(), left_viewport, 3, 4);
  EXPECT_EQ(context()->mouse->device_id(), left_mouse_id);

  auto right_viewport = left_viewport;
  right_viewport.offset_x = 0;
  platf::virtualhid::move_mouse(*context(), right_viewport, -5, 9);
  EXPECT_NE(context()->mouse->device_id(), left_mouse_id);
  mouse_event = context()->mouse->last_submitted_event();
  EXPECT_EQ(mouse_event.kind, lvh::MouseEventKind::relative_motion);
  EXPECT_EQ(mouse_event.x, -5);
  EXPECT_EQ(mouse_event.y, 9);
}

TEST_F(VirtualHidDeviceTest, TranslatesTouchscreenLifecycleAndGeometry) {
  const platf::touch_port_t viewport {10, 20, 800, 600, 400, 300, 0, 0, "DP-1"};
  platf::touch_input_t touch {LI_TOUCH_EVENT_DOWN, 100, 7, -0.1F, 1.1F, 1.2F, 15.0F, 10.0F};
  platf::virtualhid::touch_update(*client(), viewport, touch);
  auto contact = client()->touch->last_submitted_contact();
  EXPECT_EQ(contact.id, 7);
  EXPECT_FLOAT_EQ(contact.x, 0.0F);
  EXPECT_FLOAT_EQ(contact.y, 1.0F);
  EXPECT_FLOAT_EQ(contact.pressure, 1.0F);
  EXPECT_EQ(contact.orientation, 80);
  EXPECT_TRUE(contact.touching);
  EXPECT_EQ(contact.viewport.offset_x, 10);
  EXPECT_EQ(contact.viewport.output_name, "DP-1");
  EXPECT_EQ(contact.viewport.offset_y, 20);
  EXPECT_EQ(contact.viewport.width, 800);
  EXPECT_EQ(contact.viewport.height, 600);
  EXPECT_FLOAT_EQ(contact.contact_major_axis, 15.0F);
  EXPECT_FLOAT_EQ(contact.contact_minor_axis, 10.0F);
  EXPECT_TRUE(client()->active_touches.contains(7));

  touch.eventType = LI_TOUCH_EVENT_HOVER;
  touch.rotation = 300;
  touch.pointerId = 8;
  touch.pressureOrDistance = 0.25F;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  contact = client()->touch->last_submitted_contact();
  EXPECT_EQ(contact.orientation, -60);
  EXPECT_FALSE(contact.touching);
  EXPECT_TRUE(client()->active_touches.contains(8));

  touch.eventType = LI_TOUCH_EVENT_UP;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_FALSE(client()->active_touches.contains(8));
  touch.eventType = LI_TOUCH_EVENT_CANCEL;
  touch.pointerId = 7;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_FALSE(client()->active_touches.contains(7));

  touch.eventType = LI_TOUCH_EVENT_MOVE;
  touch.rotation = LI_ROT_UNKNOWN;
  touch.pointerId = 9;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_EQ(client()->touch->last_submitted_contact().orientation, 0);
  touch.eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_FALSE(client()->active_touches.contains(9));

  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 10;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  touch.pointerId = 11;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  const auto before_cancel = client()->touch->submit_count();
  touch.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_TRUE(client()->active_touches.empty());
  EXPECT_EQ(client()->touch->submit_count(), before_cancel + 2);

  const auto before_unknown = client()->touch->submit_count();
  touch.eventType = 0xFF;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  EXPECT_EQ(client()->touch->submit_count(), before_unknown);
  ASSERT_TRUE(client()->touch->close().ok());
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  platf::virtualhid::touch_update(*client(), viewport, touch);
  client()->touch.reset();
  platf::virtualhid::touch_update(*client(), viewport, touch);
}

TEST_F(VirtualHidDeviceTest, TranslatesPenButtonsToolsAndTransitions) {
  const platf::touch_port_t viewport {1, 2, 1000, 500, 1000, 500, 0, 0, "HDMI-A-1"};
  platf::pen_input_t pen {
    LI_TOUCH_EVENT_DOWN,
    LI_TOOL_TYPE_PEN,
    static_cast<std::uint8_t>(LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_SECONDARY),
    45,
    450,
    -0.25F,
    1.25F,
    1.5F,
    0.0F,
    0.0F,
  };
  platf::virtualhid::pen_update(*client(), viewport, pen);
  auto state = client()->pen->last_submitted_tool();
  EXPECT_EQ(state.tool, lvh::PenToolType::pen);
  EXPECT_FLOAT_EQ(state.x, 0.0F);
  EXPECT_FLOAT_EQ(state.y, 1.0F);
  EXPECT_FLOAT_EQ(state.pressure, 1.0F);
  EXPECT_FLOAT_EQ(state.distance, -1.0F);
  EXPECT_NEAR(state.tilt_x, -45.0F, 0.001F);
  EXPECT_NEAR(state.tilt_y, 0.0F, 0.001F);
  EXPECT_EQ(state.transition, lvh::PointerTransition::update);
  EXPECT_EQ(state.viewport.offset_x, 1);
  EXPECT_EQ(state.viewport.offset_y, 2);
  EXPECT_EQ(state.viewport.output_name, "HDMI-A-1");
  EXPECT_TRUE(client()->pressed_pen_buttons.contains(lvh::PenButton::primary));
  EXPECT_TRUE(client()->pressed_pen_buttons.contains(lvh::PenButton::secondary));

  const auto before_unchanged_buttons = client()->pen->submit_count();
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->submit_count(), before_unchanged_buttons + 1);

  pen.eventType = LI_TOUCH_EVENT_HOVER;
  pen.toolType = LI_TOOL_TYPE_ERASER;
  pen.penButtons = LI_PEN_BUTTON_TERTIARY;
  pen.tilt = LI_TILT_UNKNOWN;
  pen.rotation = LI_ROT_UNKNOWN;
  pen.pressureOrDistance = 0.4F;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  state = client()->pen->last_submitted_tool();
  EXPECT_EQ(state.tool, lvh::PenToolType::eraser);
  EXPECT_FLOAT_EQ(state.pressure, -1.0F);
  EXPECT_FLOAT_EQ(state.distance, 0.4F);
  EXPECT_FLOAT_EQ(state.tilt_x, 0.0F);
  EXPECT_FLOAT_EQ(state.tilt_y, 0.0F);
  EXPECT_TRUE(client()->pressed_pen_buttons.contains(lvh::PenButton::tertiary));
  EXPECT_FALSE(client()->pressed_pen_buttons.contains(lvh::PenButton::primary));

  pen.eventType = LI_TOUCH_EVENT_UP;
  pen.toolType = LI_TOOL_TYPE_UNKNOWN;
  pen.penButtons = 0;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->last_submitted_tool().tool, lvh::PenToolType::unchanged);
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::release);

  pen.eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::leave);
  pen.eventType = LI_TOUCH_EVENT_CANCEL;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);

  pen.eventType = LI_TOUCH_EVENT_DOWN;
  pen.penButtons = LI_PEN_BUTTON_PRIMARY;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  ASSERT_FALSE(client()->pressed_pen_buttons.empty());
  pen.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  EXPECT_TRUE(client()->pressed_pen_buttons.empty());
  EXPECT_EQ(client()->pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);

  ASSERT_TRUE(client()->pen->close().ok());
  pen.eventType = LI_TOUCH_EVENT_DOWN;
  platf::virtualhid::pen_update(*client(), viewport, pen);
  client()->pen.reset();
  platf::virtualhid::pen_update(*client(), viewport, pen);
}

TEST_F(VirtualHidDeviceTest, PlatformWrappersForwardToVirtualHidContext) {
  auto platform_input = platf::input();
  ASSERT_TRUE(platform_input);
  auto &platform_context = platf::virtualhid::get_input_context(platform_input);
  platform_context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
  ASSERT_NE(platform_context.mouse, nullptr);
  ASSERT_NE(platform_context.keyboard, nullptr);

  const platf::touch_port_t viewport {0, 0, 1280, 720, 1280, 720};
  platf::move_mouse(platform_input, 2, 3);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::relative_motion);
  platf::abs_mouse(platform_input, viewport, 40.0F, 50.0F);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::absolute_motion);
  platf::button_mouse(platform_input, BUTTON_LEFT, false);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::button);
  platf::scroll(platform_input, 120);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::vertical_scroll);
  platf::hscroll(platform_input, -120);
  EXPECT_EQ(platform_context.mouse->last_submitted_event().kind, lvh::MouseEventKind::horizontal_scroll);
  platf::keyboard_update(platform_input, 0x41, false, 0);
  EXPECT_EQ(platform_context.keyboard->last_submitted_event().key_code, 0x41);
  platf::keyboard_update(platform_input, 0x0D, false, 0, true);
  EXPECT_TRUE(platform_context.keyboard->last_submitted_event().extended);
  const std::string text = "wrapper";
  const auto keyboard_count = platform_context.keyboard->submit_count();
  platf::unicode(platform_input, text.data(), static_cast<int>(text.size()));
  EXPECT_EQ(platform_context.keyboard->submit_count(), keyboard_count + 1);

  config::input.gamepad = "ds5";
  const auto capabilities = static_cast<std::uint16_t>(LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_BATTERY_STATE);
  const platf::gamepad_id_t gamepad_id {0, 2};
  const platf::gamepad_arrival_t gamepad_metadata {LI_CTYPE_PS, capabilities, 0};
  ASSERT_EQ(platf::alloc_gamepad(platform_input, gamepad_id, gamepad_metadata, feedback_queue()), 0);
  auto *adapter = platf::virtualhid::gamepad_adapter_for_testing(platform_context, 0);
  ASSERT_NE(adapter, nullptr);
  platf::gamepad_update(platform_input, 0, {platf::A, 255, 0, 0, 0, 0, 0});
  EXPECT_TRUE(adapter->state().buttons.test(lvh::GamepadButton::a));
  platf::gamepad_touch(platform_input, {{0, 2}, LI_TOUCH_EVENT_DOWN, 1, 0.25F, 0.5F, 1.0F});
  EXPECT_TRUE(adapter->state().touchpad_contacts[0].active);
  platf::gamepad_motion(platform_input, {{0, 2}, LI_MOTION_TYPE_ACCEL, 1.0F, 2.0F, 3.0F});
  EXPECT_TRUE(adapter->state().acceleration.has_value());
  platf::gamepad_battery(platform_input, {{0, 2}, LI_BATTERY_STATE_FULL, 100});
  EXPECT_TRUE(adapter->state().battery.has_value());

  const auto &supported = platf::supported_gamepads(std::addressof(platform_input));
  ASSERT_FALSE(supported.empty());
#ifdef __APPLE__
  const auto licensed = lvh::get_license_status().license.licensed();
  EXPECT_EQ(supported.front().is_enabled, licensed);
  EXPECT_EQ(supported.front().reason_disabled, licensed ? "" : "gamepads.virtualhid-license-invalid");
#else
  EXPECT_TRUE(supported.front().is_enabled);
#endif
  EXPECT_FALSE(platf::supported_gamepads(nullptr).empty());
#ifdef _WIN32
  config::input.gamepad_driver = config::GAMEPAD_DRIVER_VIGEMBUS;
  const auto &vigembus_gamepads = platf::supported_gamepads(std::addressof(platform_input));
  config::input.gamepad_driver = config::GAMEPAD_DRIVER_ALL;
  ASSERT_EQ(vigembus_gamepads.size(), 3U);
  EXPECT_EQ(vigembus_gamepads[0].name, "auto");
  EXPECT_EQ(vigembus_gamepads[1].name, "x360");
  EXPECT_EQ(vigembus_gamepads[2].name, "ds4");
#endif
#ifdef __APPLE__
  const auto controller_touch = platf::get_capabilities() & platf::platform_caps::controller_touch;
  if (lvh::get_license_status().license.licensed()) {
    EXPECT_NE(controller_touch, 0U);
  } else {
    EXPECT_EQ(controller_touch, 0U);
  }
#else
  EXPECT_NE(platf::get_capabilities() & platf::platform_caps::controller_touch, 0U);
#endif

  platf::free_gamepad(platform_input, 0);
  EXPECT_FALSE(platf::virtualhid::has_gamepad(platform_context, 0));
  platf::free_gamepad(platform_input, 0);
  platf::gamepad_update(platform_input, 0, {});
  platf::gamepad_touch(platform_input, {{0, 2}, LI_TOUCH_EVENT_DOWN, 1, 0.25F, 0.5F, 1.0F});
  platf::gamepad_motion(platform_input, {{0, 2}, LI_MOTION_TYPE_ACCEL, 1.0F, 2.0F, 3.0F});
  platf::gamepad_battery(platform_input, {{0, 2}, LI_BATTERY_STATE_FULL, 100});

  auto platform_client = platf::allocate_client_input_context(platform_input);
  ASSERT_NE(platform_client, nullptr);
  auto &platform_client_context = platf::virtualhid::get_client_context(platform_client.get());
  ASSERT_NE(platform_client_context.touch, nullptr);
  ASSERT_NE(platform_client_context.pen, nullptr);
  platf::touch_update(platform_client.get(), viewport, {LI_TOUCH_EVENT_DOWN, 0, 1, 0.25F, 0.5F, 1.0F, 2.0F, 1.0F});
  EXPECT_EQ(platform_client_context.touch->last_submitted_contact().id, 1);
  platf::pen_update(platform_client.get(), viewport, {LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN, 0, LI_TILT_UNKNOWN, LI_ROT_UNKNOWN, 0.25F, 0.5F, 0.5F, 0.0F, 0.0F});
  EXPECT_EQ(platform_client_context.pen->last_submitted_tool().tool, lvh::PenToolType::pen);
}
