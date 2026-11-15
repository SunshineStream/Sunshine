/**
 * @file tests/unit/test_system_tray.cpp
 * @brief Tests for Sunshine's system tray integration.
 */

// test includes
#include "../tests_common.h"
#if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
  #if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    #include "third-party/tray/tests/notification_utils.h"
    #include "third-party/tray/tests/screenshot_utils.h"
  #endif
#endif

// standard includes
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

// Only test the system tray if it is enabled.
#if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1

  // lib includes
  #include <tray.h>
  #if defined(_WIN32) || defined(__APPLE__)
    #include <libvirtualhid/license.hpp>
  #endif
  #if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    #include <lizardbyte/common/env.h>
  #endif

  // local includes
  #include <src/config.h>
  #include <src/system_tray.h>
  #include <src/thread_safe.h>

namespace {
  using namespace std::chrono_literals;

  #ifndef _WIN32
  /**
   * @brief Process pending Qt tray events without blocking.
   *
   * @param iterations Maximum number of event iterations to process.
   * @param delay Delay between event iterations.
   */
  void pump_tray_events(const int iterations = 100, const std::chrono::milliseconds delay = 5ms) {
    for (int i = 0; i < iterations; ++i) {
      if (tray_loop(0) != 0) {
        return;
      }
      std::this_thread::sleep_for(delay);
    }
  }
  #else

  /**
   * @brief Wait for the threaded tray worker to finish initialization.
   *
   * @param timeout Maximum time to wait.
   * @return true if the tray initialized before the timeout; otherwise, false.
   */
  bool wait_for_tray_initialization(const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (system_tray::tray_initialized_for_testing()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
  #endif

  #ifdef _WIN32
  /**
   * @brief Verify the persistent Virtual HID Broker benefits submenu.
   *
   * @param benefits_menu Benefits submenu to verify.
   */
  void verify_virtualhid_benefits_menu(const struct tray_menu *benefits_menu) {
    ASSERT_NE(benefits_menu, nullptr);
    EXPECT_STREQ(benefits_menu[0].text, "Xbox One, Xbox Series, DualSense (DS5), Switch Pro, and Generic");
    EXPECT_EQ(benefits_menu[0].disabled, 1);
    EXPECT_STREQ(benefits_menu[1].text, "Raw Input keyboard and mouse for physical-style input");
    EXPECT_EQ(benefits_menu[1].disabled, 1);
    EXPECT_STREQ(benefits_menu[2].text, "Motion, touchpads, LEDs, and adaptive triggers where supported");
    EXPECT_EQ(benefits_menu[2].disabled, 1);
    EXPECT_STREQ(benefits_menu[3].text, "Actively developed and supported by LizardByte");
    EXPECT_EQ(benefits_menu[3].disabled, 1);
    EXPECT_EQ(benefits_menu[4].text, nullptr);
  }

  /**
   * @brief Verify the shared action entries in a populated Virtual HID Broker menu.
   *
   * @param license_menu License submenu to verify.
   */
  void verify_virtualhid_actions_menu(const struct tray_menu *license_menu) {
    ASSERT_NE(license_menu, nullptr);
    EXPECT_STREQ(license_menu[4].text, "-");
    EXPECT_STREQ(license_menu[5].text, "Get/Manage License");
    EXPECT_NE(license_menu[5].cb, nullptr);
    EXPECT_STREQ(license_menu[6].text, "Virtual HID Broker Benefits");
    EXPECT_EQ(license_menu[6].cb, nullptr);
    verify_virtualhid_benefits_menu(license_menu[6].submenu);
    EXPECT_STREQ(license_menu[7].text, "Download Virtual HID Broker");
    EXPECT_NE(license_menu[7].cb, nullptr);
    EXPECT_EQ(license_menu[8].text, nullptr);
  }
  #endif

  /**
   * @brief Verify the persistent menu exposed by Sunshine.
   */
  void verify_menu() {
    const auto &tray_data = system_tray::tray_data_for_testing();
    ASSERT_NE(tray_data.menu, nullptr);

    EXPECT_STREQ(tray_data.menu[0].text, "Open Sunshine");
    EXPECT_NE(tray_data.menu[0].cb, nullptr);
    EXPECT_STREQ(tray_data.menu[1].text, "-");
    EXPECT_EQ(tray_data.menu[1].cb, nullptr);

  #if defined(_WIN32) || defined(__APPLE__)
    EXPECT_STREQ(tray_data.menu[2].text, "Virtual HID Broker");
    ASSERT_NE(tray_data.menu[2].submenu, nullptr);
    EXPECT_STREQ(tray_data.menu[2].submenu[0].text, "Status: Checking");
    EXPECT_EQ(tray_data.menu[2].submenu[0].disabled, 1);
    EXPECT_STREQ(tray_data.menu[2].submenu[1].text, "-");
    EXPECT_STREQ(tray_data.menu[2].submenu[2].text, "Get/Manage License");
    EXPECT_NE(tray_data.menu[2].submenu[2].cb, nullptr);
    #ifdef _WIN32
    EXPECT_STREQ(tray_data.menu[2].submenu[3].text, "Virtual HID Broker Benefits");
    EXPECT_EQ(tray_data.menu[2].submenu[3].cb, nullptr);
    verify_virtualhid_benefits_menu(tray_data.menu[2].submenu[3].submenu);
    EXPECT_STREQ(tray_data.menu[2].submenu[4].text, "Download Virtual HID Broker");
    EXPECT_NE(tray_data.menu[2].submenu[4].cb, nullptr);
    EXPECT_EQ(tray_data.menu[2].submenu[5].text, nullptr);
    #else
    EXPECT_STREQ(tray_data.menu[2].submenu[3].text, "Download Virtual HID Broker");
    EXPECT_NE(tray_data.menu[2].submenu[3].cb, nullptr);
    EXPECT_EQ(tray_data.menu[2].submenu[4].text, nullptr);
    #endif
    EXPECT_STREQ(tray_data.menu[3].text, "-");
    EXPECT_STREQ(tray_data.menu[4].text, "Donate");
    ASSERT_NE(tray_data.menu[4].submenu, nullptr);
    EXPECT_STREQ(tray_data.menu[4].submenu[0].text, "GitHub Sponsors");
    EXPECT_STREQ(tray_data.menu[4].submenu[1].text, "Patreon");
    EXPECT_STREQ(tray_data.menu[4].submenu[2].text, "PayPal");
    EXPECT_EQ(tray_data.menu[4].submenu[3].text, nullptr);
    EXPECT_STREQ(tray_data.menu[5].text, "-");
    #ifdef _WIN32
    EXPECT_STREQ(tray_data.menu[6].text, "Reset Display Device Config");
    EXPECT_NE(tray_data.menu[6].cb, nullptr);
    EXPECT_STREQ(tray_data.menu[7].text, "Restart");
    EXPECT_NE(tray_data.menu[7].cb, nullptr);
    EXPECT_STREQ(tray_data.menu[8].text, "Quit");
    EXPECT_NE(tray_data.menu[8].cb, nullptr);
    EXPECT_EQ(tray_data.menu[9].text, nullptr);
    #else
    EXPECT_STREQ(tray_data.menu[6].text, "Restart");
    EXPECT_NE(tray_data.menu[6].cb, nullptr);
    EXPECT_STREQ(tray_data.menu[7].text, "Quit");
    EXPECT_NE(tray_data.menu[7].cb, nullptr);
    EXPECT_EQ(tray_data.menu[8].text, nullptr);
    #endif
  #else
    EXPECT_STREQ(tray_data.menu[2].text, "Donate");
    ASSERT_NE(tray_data.menu[2].submenu, nullptr);
    EXPECT_STREQ(tray_data.menu[2].submenu[0].text, "GitHub Sponsors");
    EXPECT_STREQ(tray_data.menu[2].submenu[1].text, "Patreon");
    EXPECT_STREQ(tray_data.menu[2].submenu[2].text, "PayPal");
    EXPECT_EQ(tray_data.menu[2].submenu[3].text, nullptr);
    EXPECT_STREQ(tray_data.menu[3].text, "-");
    EXPECT_STREQ(tray_data.menu[4].text, "Restart");
    EXPECT_NE(tray_data.menu[4].cb, nullptr);
    EXPECT_STREQ(tray_data.menu[5].text, "Quit");
    EXPECT_NE(tray_data.menu[5].cb, nullptr);
    EXPECT_EQ(tray_data.menu[6].text, nullptr);
  #endif
  }

  /**
   * @brief Verify the current user-visible tray state.
   *
   * @param icon_index Index of the expected tray icon.
   * @param tooltip Expected tray tooltip.
   * @param notification_title Expected notification title, or nullptr when none is expected.
   * @param notification_text Expected notification text, or nullptr when none is expected.
   * @param notification_icon_index Index of the expected notification icon, or no value when none is expected.
   * @param expects_notification_callback Whether a notification callback is expected.
   */
  void verify_state(
    const std::size_t icon_index,
    const char *tooltip,
    const char *notification_title,
    const char *notification_text,
    const std::optional<std::size_t> notification_icon_index,
    const bool expects_notification_callback
  ) {
    const auto &tray_data = system_tray::tray_data_for_testing();
    ASSERT_GE(tray_data.iconPathCount, 4);

    EXPECT_STREQ(tray_data.icon, tray_data.allIconPaths[icon_index]);
    EXPECT_STREQ(tray_data.tooltip, tooltip);
    EXPECT_STREQ(tray_data.notification_title, notification_title);
    EXPECT_STREQ(tray_data.notification_text, notification_text);
    if (notification_icon_index.has_value()) {
      EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[*notification_icon_index]);
    } else {
      EXPECT_EQ(tray_data.notification_icon, nullptr);
    }
    if (expects_notification_callback) {
      EXPECT_NE(tray_data.notification_cb, nullptr);
    } else {
      EXPECT_EQ(tray_data.notification_cb, nullptr);
    }
  }

  /**
   * @brief Verify every user-visible tray state transition.
   */
  #ifndef _WIN32
  void verify_state_transitions() {
    verify_state(0, PROJECT_NAME, nullptr, nullptr, std::nullopt, false);

    system_tray::update_tray_playing("Moonlight");
    verify_state(2, "Streaming started for Moonlight", "Stream Started", "Streaming started for Moonlight", 2, false);

    system_tray::update_tray_pausing("Moonlight");
    verify_state(3, "Streaming paused for Moonlight", "Stream Paused", "Streaming paused for Moonlight", 3, false);

    system_tray::update_tray_stopped("Moonlight");
    verify_state(0, PROJECT_NAME, "Application Stopped", "Application Moonlight successfully stopped", 0, false);

    system_tray::update_tray_require_pin();
    verify_state(0, PROJECT_NAME, "Incoming Pairing Request", "Click here to complete the pairing process", 1, true);
  }
  #endif
}  // namespace

/**
 * @brief Fixture that resets the process-wide tray state between tests.
 */
class SystemTrayTest: public testing::Test {
protected:
  /**
   * @brief Reset any tray state left by an earlier test.
   */
  void SetUp() override {
  #ifdef _WIN32
    original_gamepad_driver_ = config::input.gamepad_driver;
    config::input.gamepad_driver = config::GAMEPAD_DRIVER_ALL;
  #elif defined(__APPLE__)
    original_controller_ = config::input.controller;
    original_gamepad_driver_ = config::input.gamepad_driver;
    config::input.controller = true;
    config::input.gamepad_driver = config::GAMEPAD_DRIVER_VIRTUALHID;
  #endif
    EXPECT_EQ(system_tray::end_tray(), 0);
  #ifndef _WIN32
    std::ignore = tray_loop(0);
    std::ignore = tray_restore_mouse_position();
  #endif
    system_tray::reset_tray_data_for_testing();
  }

  /**
   * @brief Shut down and reset the tray after a test.
   */
  void TearDown() override {
  #if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    dismissNativeNotifications();
  #endif
    EXPECT_EQ(system_tray::end_tray(), 0);
  #ifdef _WIN32
    std::this_thread::sleep_for(250ms);
  #endif
  #ifndef _WIN32
    std::ignore = tray_loop(0);
    std::ignore = tray_restore_mouse_position();
  #endif
    system_tray::reset_tray_data_for_testing();
  #ifdef _WIN32
    config::input.gamepad_driver = std::move(original_gamepad_driver_);
  #elif defined(__APPLE__)
    config::input.controller = original_controller_;
    config::input.gamepad_driver = std::move(original_gamepad_driver_);
  #endif
  }

  /**
   * @brief Initialize the tray and process its initial events.
   *
   * @return The result returned by system_tray::init_tray().
   */
  int initialize_tray() const {
    const int result = system_tray::init_tray();
  #ifndef _WIN32
    if (result == 0) {
      pump_tray_events();
    }
  #endif
    return result;
  }

private:
  #ifdef _WIN32
  std::string original_gamepad_driver_;  ///< Original gamepad driver preference restored after each test.
  #elif defined(__APPLE__)
  bool original_controller_ = false;  ///< Original gamepad input setting restored after each test.
  std::string original_gamepad_driver_;  ///< Original gamepad backend setting restored after each test.
  #endif
};

TEST_F(SystemTrayTest, UpdatesAreIgnoredBeforeInitialization) {
  EXPECT_FALSE(system_tray::tray_initialized_for_testing());
  EXPECT_EQ(system_tray::process_tray_events(), 1);

  system_tray::update_tray_playing("Moonlight");
  system_tray::update_tray_pausing("Moonlight");
  system_tray::update_tray_stopped("Moonlight");
  system_tray::update_tray_require_pin();

  const auto &tray_data = system_tray::tray_data_for_testing();
  EXPECT_STREQ(tray_data.icon, tray_data.allIconPaths[0]);
  EXPECT_STREQ(tray_data.tooltip, PROJECT_NAME);
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);
  EXPECT_EQ(tray_data.notification_icon, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);
  EXPECT_EQ(system_tray::end_tray(), 0);
}

  #ifdef __APPLE__
TEST_F(SystemTrayTest, LeftClickUsesNativeMacOSMenu) {
  const auto &tray_data = system_tray::tray_data_for_testing();
  ASSERT_NE(tray_data.cb, nullptr);
  ASSERT_NE(tray_data.menu, nullptr);

  tray_data.cb(nullptr);

  EXPECT_STREQ(tray_data.menu[0].text, "Open Sunshine");
  EXPECT_FALSE(system_tray::tray_initialized_for_testing());
}

TEST_F(SystemTrayTest, UsesMonochromeMacOSMenuBarIconsForEveryState) {
  const auto &tray_data = system_tray::tray_data_for_testing();
  ASSERT_EQ(tray_data.iconPathCount, 5);
  for (const auto *name : {"sunshine", "playing", "pausing", "locked"}) {
    const std::string suffix = std::format("images/macos-tray-{}.svg", name);
    bool found = false;
    for (int index = 0; index < 4; ++index) {
      if (std::string_view {tray_data.allIconPaths[index]}.ends_with(suffix)) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "Missing macOS menu bar icon: " << suffix;
  }
}
  #endif

  #ifdef _WIN32
TEST_F(SystemTrayTest, ResolvesDevelopmentTrayIconsFromExecutableDirectory) {
  EXPECT_EQ(system_tray::resource_path_for_testing(nullptr), nullptr);
  EXPECT_EQ(system_tray::resource_path_for_testing(""), nullptr);

  const auto *sunshine_icon = system_tray::resource_path_for_testing("test_assets/web/images/logo-sunshine.svg");
  ASSERT_NE(sunshine_icon, nullptr);
  EXPECT_TRUE(std::filesystem::path {sunshine_icon}.is_absolute());
  EXPECT_TRUE(std::filesystem::exists(sunshine_icon));
  EXPECT_EQ(system_tray::resource_path_for_testing("test_assets/web/images/logo-sunshine.svg"), sunshine_icon);

  const auto *virtualhid_icon = system_tray::resource_path_for_testing("test_assets/web/images/logo-libvirtualhid.svg");
  ASSERT_NE(virtualhid_icon, nullptr);
  EXPECT_TRUE(std::filesystem::path {virtualhid_icon}.is_absolute());
  EXPECT_TRUE(std::filesystem::exists(virtualhid_icon));
}

TEST_F(SystemTrayTest, PreparesVirtualHidMenuFromCurrentLicenseStatus) {
  system_tray::prepare_tray_virtualhid_license();

  const auto &tray_data = system_tray::tray_data_for_testing();
  ASSERT_NE(tray_data.menu[2].submenu, nullptr);
  ASSERT_NE(tray_data.menu[2].submenu[0].text, nullptr);
  EXPECT_STRNE(tray_data.menu[2].submenu[0].text, "Status: Checking");
}

TEST_F(SystemTrayTest, PreparesLicensedVirtualHidMenuBeforeInitialization) {
  lvh::LicenseStatus license;
  license.service_available = true;
  license.state = lvh::LicenseState::licensed;
  license.plan_name = "Yearly";
  license.customer_email = "customer@example.com";
  license.activation_usage = 1;
  license.activation_limit = 5;

  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  const auto *license_menu = tray_data.menu[2].submenu;
  ASSERT_NE(license_menu, nullptr);
  EXPECT_STREQ(license_menu[0].text, "Status: Licensed");
  EXPECT_STREQ(license_menu[1].text, "Plan: Yearly");
  EXPECT_STREQ(license_menu[2].text, "Customer: customer@example.com");
  EXPECT_STREQ(license_menu[3].text, "Machine activation limit: 5");
  EXPECT_STREQ(license_menu[4].text, "-");
  EXPECT_STREQ(license_menu[5].text, "Get/Manage License");
  EXPECT_NE(license_menu[5].cb, nullptr);
  verify_virtualhid_actions_menu(license_menu);
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);

  license.plan_name.clear();
  license.customer_email.clear();
  license.activation_usage = 0;
  license.activation_limit = 0;
  system_tray::update_tray_virtualhid_license(license, false);

  EXPECT_STREQ(license_menu[1].text, "This machine is activated");
  EXPECT_STREQ(license_menu[2].text, "Customer: Not reported");
  EXPECT_STREQ(license_menu[3].text, "Machine activation limit: Not reported");
}

TEST_F(SystemTrayTest, PromptsForUnsetGamepadDriverEvenWhenLicensed) {
  lvh::LicenseStatus license;
  license.service_available = true;
  license.state = lvh::LicenseState::licensed;
  config::input.gamepad_driver.clear();

  system_tray::update_tray_virtualhid_license(license, false);

  const auto &tray_data = system_tray::tray_data_for_testing();
  EXPECT_STREQ(tray_data.notification_title, "Choose a Gamepad Backend");
  EXPECT_STREQ(
    tray_data.notification_text,
    "Choose a gamepad backend in Input settings. Virtual HID Broker is a paid upgrade; ViGEmBus is limited and has reached end of life."
  );
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
  EXPECT_NE(tray_data.notification_cb, nullptr);
}

TEST_F(SystemTrayTest, NotifiesWhenVirtualHidDriverIsUnsupported) {
  const auto supported_versions = std::format(">= {}", LIBVIRTUALHID_MINIMUM_VERSION);
  system_tray::update_tray_virtualhid_driver(true, "2026.829.2338.54", false, supported_versions);  // NOSONAR(cpp:S1313): not an IP address

  const auto &tray_data = system_tray::tray_data_for_testing();
  const auto expected_notification = std::format(
    "Installed Virtual HID Broker v2026.829.2338.54 is not supported by this version of Sunshine. Supported versions: {}. Restart Sunshine after updating. Click for instructions.",
    supported_versions
  );
  EXPECT_STREQ(tray_data.notification_title, "Update Virtual HID Broker");
  EXPECT_STREQ(tray_data.notification_text, expected_notification.c_str());
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
  EXPECT_NE(tray_data.notification_cb, nullptr);

  system_tray::resolve_tray_icon_paths_for_testing();
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
}

TEST_F(SystemTrayTest, PreservesExistingNotificationWhenVirtualHidDriverIsUsable) {
  lvh::LicenseStatus license;
  license.service_available = true;
  license.state = lvh::LicenseState::unlicensed;
  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  const auto *notification_title = tray_data.notification_title;
  const auto *notification_text = tray_data.notification_text;
  const auto notification_callback = tray_data.notification_cb;

  const auto supported_versions = std::format(">= {}", LIBVIRTUALHID_MINIMUM_VERSION);
  system_tray::update_tray_virtualhid_driver(false, "", false, supported_versions);
  EXPECT_EQ(tray_data.notification_title, notification_title);
  EXPECT_EQ(tray_data.notification_text, notification_text);
  EXPECT_EQ(tray_data.notification_cb, notification_callback);

  system_tray::update_tray_virtualhid_driver(true, "0.0.1", true, supported_versions);
  EXPECT_EQ(tray_data.notification_title, notification_title);
  EXPECT_EQ(tray_data.notification_text, notification_text);
  EXPECT_EQ(tray_data.notification_cb, notification_callback);
}

TEST_F(SystemTrayTest, VigembusSelectionSuppressesVirtualHidNotifications) {
  lvh::LicenseStatus license;
  license.service_available = true;
  license.state = lvh::LicenseState::invalid;
  config::input.gamepad_driver = config::GAMEPAD_DRIVER_VIGEMBUS;

  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);

  const auto supported_versions = std::format(">= {}", LIBVIRTUALHID_MINIMUM_VERSION);
  system_tray::update_tray_virtualhid_driver(true, "2026.829.2338.54", false, supported_versions);  // NOSONAR(cpp:S1313): not IP addresses
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);
}

TEST_F(SystemTrayTest, NoneSelectionSuppressesWindowsVirtualHidNotifications) {
  lvh::LicenseStatus license;
  license.service_available = false;
  license.state = lvh::LicenseState::unavailable;
  config::input.gamepad_driver = config::GAMEPAD_DRIVER_NONE;

  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);
  EXPECT_STREQ(tray_data.menu[2].submenu[0].text, "Status: Unavailable");

  const auto supported_versions = std::format(">= {}", LIBVIRTUALHID_MINIMUM_VERSION);
  system_tray::update_tray_virtualhid_driver(true, "2026.829.2338.54", false, supported_versions);  // NOSONAR(cpp:S1313): not IP addresses
  EXPECT_EQ(tray_data.notification_title, nullptr);
}

/**
 * @brief License-state fixture for the Windows Virtual HID Broker tray menu.
 */
class UnlicensedVirtualHidTrayTest:
    public SystemTrayTest,
    public testing::WithParamInterface<std::tuple<lvh::LicenseState, const char *, const char *, bool>> {};

TEST_P(UnlicensedVirtualHidTrayTest, PreparesMenuAndStartupNotification) {
  const auto &[state, state_label, state_detail, service_available] = GetParam();
  lvh::LicenseStatus license;
  license.service_available = service_available;
  license.state = state;

  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  const auto *license_menu = tray_data.menu[2].submenu;
  ASSERT_NE(license_menu, nullptr);
  EXPECT_STREQ(license_menu[0].text, std::format("Status: {}", state_label).c_str());
  EXPECT_STREQ(license_menu[1].text, state_detail);
  EXPECT_STREQ(license_menu[2].text, "Driver-backed keyboard, mouse, and gamepads are locked");
  EXPECT_STREQ(license_menu[3].text, service_available ? "License service: Available" : "License service: Unavailable");
  verify_virtualhid_actions_menu(license_menu);
  EXPECT_STREQ(tray_data.notification_title, "Virtual HID Broker License");
  EXPECT_STREQ(
    tray_data.notification_text,
    "Get or manage a license, or use the limited, end-of-life ViGEmBus driver."
  );
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
  EXPECT_NE(tray_data.notification_cb, nullptr);

  system_tray::resolve_tray_icon_paths_for_testing();
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);

  system_tray::update_tray_virtualhid_license(license, false);
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);
}

INSTANTIATE_TEST_SUITE_P(
  LicenseStates,
  UnlicensedVirtualHidTrayTest,
  testing::Values(
    std::tuple {lvh::LicenseState::unavailable, "Unavailable", "The local license service is unavailable", false},
    std::tuple {lvh::LicenseState::unlicensed, "Not Activated", "No license is active on this machine", true},
    std::tuple {lvh::LicenseState::expired, "Expired", "The license on this machine has expired", true},
    std::tuple {lvh::LicenseState::disabled, "Disabled", "The license on this machine is disabled", true},
    std::tuple {lvh::LicenseState::invalid, "Invalid", "The license on this machine is invalid", true}
  )
);
  #endif

  #ifdef __APPLE__
TEST_F(SystemTrayTest, NotifiesWhenMacBrokerVersionIsUnsupported) {
  const auto supported_versions = std::format(">= {}", LIBVIRTUALHID_MACOS_MINIMUM_VERSION);
  system_tray::update_tray_virtualhid_driver(true, "2026.829.2338", false, supported_versions);  // NOSONAR(cpp:S1313): not an IP address

  const auto &tray_data = system_tray::tray_data_for_testing();
  const auto expected_notification = std::format(
    "Installed Virtual HID Broker v2026.829.2338 is not supported by this version of Sunshine. Supported versions: {}. Restart Sunshine after updating. Click for instructions.",
    supported_versions
  );
  EXPECT_STREQ(tray_data.notification_title, "Update Virtual HID Broker");
  EXPECT_STREQ(tray_data.notification_text, expected_notification.c_str());
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
  EXPECT_NE(tray_data.notification_cb, nullptr);
}

TEST_F(SystemTrayTest, MacBrokerVersionNotificationRespectsDisabledGamepads) {
  const auto supported_versions = std::format(">= {}", LIBVIRTUALHID_MACOS_MINIMUM_VERSION);
  config::input.gamepad_driver = config::GAMEPAD_DRIVER_NONE;
  system_tray::update_tray_virtualhid_driver(true, "2026.829.2338", false, supported_versions);  // NOSONAR(cpp:S1313): not an IP address

  const auto &tray_data = system_tray::tray_data_for_testing();
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_text, nullptr);

  config::input.gamepad_driver.clear();
  system_tray::update_tray_virtualhid_driver(true, "2026.829.2338", false, supported_versions);  // NOSONAR(cpp:S1313): not an IP address
  EXPECT_STREQ(tray_data.notification_title, "Update Virtual HID Broker");
}

TEST_F(SystemTrayTest, ShowsLicensedMacBrokerDetailsBeforeInitialization) {
  lvh::LicenseStatus license;
  license.service_available = true;
  license.state = lvh::LicenseState::licensed;
  license.plan_name = "Yearly";
  license.customer_email = "customer@example.com";
  license.activation_usage = 1;
  license.activation_limit = 5;

  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  ASSERT_EQ(tray_data.iconPathCount, 5);
  const auto *menu = tray_data.menu[2].submenu;
  ASSERT_NE(menu, nullptr);
  EXPECT_STREQ(menu[0].text, "Status: Licensed");
  EXPECT_STREQ(menu[1].text, "Plan: Yearly");
  EXPECT_STREQ(menu[2].text, "Customer: customer@example.com");
  EXPECT_STREQ(menu[3].text, "Machine activation limit: 5");
  EXPECT_STREQ(menu[4].text, "-");
  EXPECT_STREQ(menu[5].text, "Get/Manage License");
  EXPECT_NE(menu[5].cb, nullptr);
  EXPECT_STREQ(menu[6].text, "Download Virtual HID Broker");
  EXPECT_NE(menu[6].cb, nullptr);
  EXPECT_EQ(menu[7].text, nullptr);
  EXPECT_EQ(tray_data.notification_title, nullptr);
}

TEST_F(SystemTrayTest, ShowsMacBrokerLicenseAndAvailabilityWithOptionalStartupNotifications) {
  lvh::LicenseStatus license;
  license.service_available = true;
  license.state = lvh::LicenseState::unlicensed;

  system_tray::update_tray_virtualhid_license(license, true);

  const auto &tray_data = system_tray::tray_data_for_testing();
  const auto *menu = tray_data.menu[2].submenu;
  ASSERT_NE(menu, nullptr);
  EXPECT_STREQ(menu[0].text, "Status: Not Activated");
  EXPECT_STREQ(menu[1].text, "No license is active on this machine");
  EXPECT_STREQ(menu[2].text, "Virtual gamepads are locked");
  EXPECT_STREQ(menu[3].text, "License service: Available");
  EXPECT_STREQ(tray_data.notification_title, "Virtual HID Broker License");
  EXPECT_STREQ(tray_data.notification_text, "Activate a machine license to use virtual gamepads. Click to manage the license.");
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
  EXPECT_NE(tray_data.notification_cb, nullptr);

  license.service_available = false;
  license.state = lvh::LicenseState::unavailable;
  system_tray::update_tray_virtualhid_license(license, true);
  EXPECT_STREQ(menu[0].text, "Status: Unavailable");
  EXPECT_STREQ(menu[3].text, "License service: Unavailable");
  EXPECT_STREQ(tray_data.notification_title, "Virtual HID Broker Is Unavailable");
  EXPECT_STREQ(tray_data.notification_text, "Install and start Virtual HID Broker to use virtual gamepads. Click to download.");
  EXPECT_STREQ(tray_data.notification_icon, tray_data.allIconPaths[4]);
  EXPECT_NE(tray_data.notification_cb, nullptr);

  system_tray::update_tray_virtualhid_license(license, false);
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);

  config::input.gamepad_driver = config::GAMEPAD_DRIVER_NONE;
  system_tray::update_tray_virtualhid_license(license, true);
  EXPECT_EQ(tray_data.notification_title, nullptr);
  EXPECT_EQ(tray_data.notification_cb, nullptr);
  EXPECT_STREQ(menu[0].text, "Status: Unavailable");

  config::input.gamepad_driver = config::GAMEPAD_DRIVER_VIRTUALHID;
  config::input.controller = false;
  system_tray::update_tray_virtualhid_license(license, true);
  EXPECT_EQ(tray_data.notification_title, nullptr);
}
  #endif

/**
 * @brief Verify the tray fallback waits for shutdown using an explicitly owned worker thread.
 */
TEST_F(SystemTrayTest, KeepsWorkersRunningWhenTrayIsUnavailable) {
  ASSERT_FALSE(system_tray::tray_initialized_for_testing());
  auto shutdown_event = std::make_shared<safe::event_t<bool>>();
  std::promise<void> tray_loop_exited;
  auto tray_loop = tray_loop_exited.get_future();
  std::jthread tray_worker([shutdown_event, &tray_loop_exited]() {
    system_tray::run_tray_until_exit(shutdown_event);
    tray_loop_exited.set_value();
  });

  EXPECT_EQ(tray_loop.wait_for(100ms), std::future_status::timeout);
  EXPECT_FALSE(shutdown_event->view(0ms));

  shutdown_event->raise(true);
  EXPECT_EQ(tray_loop.wait_for(5s), std::future_status::ready);
  tray_worker.join();
  EXPECT_TRUE(shutdown_event->peek());
  EXPECT_FALSE(system_tray::tray_initialized_for_testing());
}

TEST_F(SystemTrayTest, HonorsPendingShutdownWhenTrayIsUnavailable) {
  ASSERT_FALSE(system_tray::tray_initialized_for_testing());
  auto shutdown_event = std::make_shared<safe::event_t<bool>>();
  shutdown_event->raise(true);

  system_tray::run_tray_until_exit(shutdown_event);

  EXPECT_TRUE(shutdown_event->peek());
  EXPECT_FALSE(system_tray::tray_initialized_for_testing());
}

  #ifndef _WIN32
TEST_F(SystemTrayTest, LifecycleMenuAndStateTransitions) {
  if (const int result = initialize_tray(); result != 0) {
    GTEST_SKIP() << "System tray is unavailable in this environment (code " << result << ")";
  }

  EXPECT_TRUE(system_tray::tray_initialized_for_testing());
  verify_menu();
  verify_state_transitions();

  std::jthread exit_thread([]() {
    std::this_thread::sleep_for(100ms);
    std::ignore = system_tray::end_tray();
  });
  auto shutdown_event = std::make_shared<safe::event_t<bool>>();
  EXPECT_FALSE(shutdown_event->peek());
  system_tray::run_tray_until_exit(shutdown_event);
  EXPECT_TRUE(shutdown_event->peek());
  exit_thread.join();
  EXPECT_FALSE(system_tray::tray_initialized_for_testing());
}
  #endif

  #ifdef _WIN32
TEST_F(SystemTrayTest, InitializesTrayForWorkflowConfiguration) {
  std::string configure_tray_icons;
  if (!lizardbyte::common::get_env("SUNSHINE_CONFIGURE_TRAY_ICONS", configure_tray_icons)) {
    GTEST_SKIP() << "Only required while configuring Windows runner tray icon visibility";
  }

  ASSERT_EQ(system_tray::init_tray_threaded(), 0);
  ASSERT_TRUE(wait_for_tray_initialization(10s));
  verify_menu();
  EXPECT_EQ(system_tray::end_tray(), 0);
}
  #endif

  #if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
/**
 * @brief Fixture that stores visual tray evidence in the test output directory.
 */
class SystemTrayVisualTest: public SystemTrayTest {
protected:
  /**
   * @brief Initialize a clean screenshot output directory for this test suite.
   */
  static void SetUpTestSuite() {
    const std::filesystem::path test_binary_dir {SUNSHINE_TEST_BIN_DIR};
    std::error_code remove_error;
    std::filesystem::remove_all(test_binary_dir / "screenshots", remove_error);
    screenshot::initialize(test_binary_dir);
  }

  /**
   * @brief Wait for a tray notification to be rendered.
   */
  static void wait_for_notification() {
    #ifndef _WIN32
    pump_tray_events();
    #endif
    #if defined(_WIN32) || defined(__APPLE__)
    if (lizardbyte::common::is_github_actions()) {
      #ifdef _WIN32
      std::this_thread::sleep_for(2s);
      #else
      pump_tray_events(40, 50ms);
      #endif
    }
    #endif
    std::this_thread::sleep_for(500ms);
  }

  /**
   * @brief Capture a screenshot and dismiss its native notification.
   *
   * @param name Screenshot filename without the PNG extension.
   */
  static void capture_notification(const std::string &name) {
    wait_for_notification();
    EXPECT_TRUE(screenshot::capture(name));
    waitForNativeNotificationTimeout();
  }

  /**
   * @brief Initialize the tray on the platform's production execution path.
   *
   * @return Zero when initialization succeeds; otherwise, a non-zero error code.
   */
  int initialize_visual_tray() const {
    #ifdef _WIN32
    if (const int result = system_tray::init_tray_threaded(); result != 0) {
      return result;
    }
    return wait_for_tray_initialization(10s) ? 0 : 1;
    #else
    return initialize_tray();
    #endif
  }
};

TEST_F(SystemTrayVisualTest, CapturesIconTooltipNotificationsAndMenu) {
  std::string unavailable_reason;
  if (!screenshot::is_available(&unavailable_reason)) {
    GTEST_SKIP() << "Screenshot tooling is unavailable: " << unavailable_reason;
  }

  const auto &tray_data = system_tray::tray_data_for_testing();
    #if defined(_WIN32) || defined(__APPLE__)
  ASSERT_EQ(tray_data.iconPathCount, 5);
    #else
  ASSERT_EQ(tray_data.iconPathCount, 4);
    #endif
  for (int icon_index = 0; icon_index < tray_data.iconPathCount; ++icon_index) {
    ASSERT_NE(tray_data.allIconPaths[icon_index], nullptr);
    ASSERT_TRUE(std::filesystem::is_regular_file(tray_data.allIconPaths[icon_index]))
      << "Missing system tray test icon: " << tray_data.allIconPaths[icon_index];
  }

  if (const int result = initialize_visual_tray(); result != 0) {
    GTEST_SKIP() << "System tray is unavailable in this environment (code " << result << ")";
  }

  verify_menu();
  verify_state(0, PROJECT_NAME, nullptr, nullptr, std::nullopt, false);
  EXPECT_TRUE(screenshot::capture("sunshine_tray_initial"));

  const int tooltip_position_result = tray_position_mouse_over_icon();
  if (lizardbyte::common::is_github_actions()) {
    ASSERT_EQ(tooltip_position_result, 0);
  }
  if (tooltip_position_result == 0) {
    #ifdef _WIN32
    std::this_thread::sleep_for(1s);
    #else
    pump_tray_events(20, 50ms);
    #endif
    EXPECT_TRUE(screenshot::capture("sunshine_tray_tooltip"));
    EXPECT_EQ(tray_restore_mouse_position(), 0);
  }

  dismissNativeNotifications();
  system_tray::update_tray_playing("Moonlight");
  verify_state(2, "Streaming started for Moonlight", "Stream Started", "Streaming started for Moonlight", 2, false);
  capture_notification("sunshine_tray_streaming");
  system_tray::update_tray_pausing("Moonlight");
  verify_state(3, "Streaming paused for Moonlight", "Stream Paused", "Streaming paused for Moonlight", 3, false);
  capture_notification("sunshine_tray_paused");
  system_tray::update_tray_stopped("Moonlight");
  verify_state(0, PROJECT_NAME, "Application Stopped", "Application Moonlight successfully stopped", 0, false);
  capture_notification("sunshine_tray_stopped");
  system_tray::update_tray_require_pin();
  verify_state(0, PROJECT_NAME, "Incoming Pairing Request", "Click here to complete the pairing process", 1, true);
  capture_notification("sunshine_tray_pairing_request");

  int menu_position_result = -1;
  if (lizardbyte::common::is_github_actions()) {
    #ifdef _WIN32
    std::this_thread::sleep_for(500ms);
    #else
    pump_tray_events();
    #endif
    menu_position_result = tray_position_mouse_over_icon();
    ASSERT_EQ(menu_position_result, 0);
  }

  std::atomic_bool capture_result {false};
  std::jthread capture_thread([&capture_result]() {
    capture_result.store(screenshot::capture("sunshine_tray_menu"), std::memory_order_release);
    std::ignore = system_tray::end_tray();
  });

  tray_show_menu();
  if (menu_position_result == 0) {
    EXPECT_EQ(tray_restore_mouse_position(), 0);
  }
    #ifdef _WIN32
  capture_thread.join();
    #else
  EXPECT_NE(system_tray::process_tray_events(), 0);
  capture_thread.join();
    #endif
  EXPECT_TRUE(capture_result.load(std::memory_order_acquire));
}
  #endif

#else

TEST(SystemTrayDisabled, ReportsDisabledBuild) {
  GTEST_SKIP() << "System tray support is disabled in this build";
}

#endif
