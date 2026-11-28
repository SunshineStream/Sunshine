/**
 * @file tests/unit/platform/linux/test_misc.cpp
 * @brief Tests for Linux platform utility functions.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"
  #include "../../../tests_log_checker.h"

  // standard includes
  #include <array>
  #include <cstdlib>
  #include <optional>
  #include <string>

  // local includes
  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/common.h"
  #include "src/platform/linux/misc.h"
  #include "src/utility.h"

namespace {
  /**
   * @brief Restore one environment variable when a test scope ends.
   */
  class environment_guard_t {
  public:
    /**
     * @brief Save the current value of an environment variable.
     *
     * @param name Environment variable name to restore.
     */
    explicit environment_guard_t(const char *name):
        name_ {name} {
      if (const char *value = std::getenv(name)) {
        value_ = value;
      }
    }

    /**
     * @brief Restore the saved value or remove the variable when it was originally absent.
     */
    ~environment_guard_t() {
      if (value_) {
        setenv(name_.c_str(), value_->c_str(), 1);
      } else {
        unsetenv(name_.c_str());
      }
    }

  private:
    std::string name_;  ///< Environment variable name.
    std::optional<std::string> value_;  ///< Value present before the test changed the environment.
  };

  constexpr std::array unsafe_environment_variables {
    "GDK_PIXBUF_MODULEDIR",
    "GDK_PIXBUF_MODULE_FILE",
    "GIO_EXTRA_MODULES",
    "GTK3_MODULES",
    "GTK_EXE_PREFIX",
    "GTK_IM_MODULE_FILE",
    "GTK_MODULES",
    "GTK_PATH",
    "QML2_IMPORT_PATH",
    "QML_IMPORT_PATH",
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
  };

  /**
   * @brief Synchronize asynchronous test logging before changing the process environment.
   *
   * GoogleTest emits test-start records before fixture setup. Draining those records prevents the logging formatter
   * from reading environment-dependent state concurrently with `setenv()` or `unsetenv()` on older glibc releases.
   */
  class ProcessEnvironmentSecurity: public testing::Test {
  protected:
    /**
     * @brief Wait for pending test-start log records to finish formatting.
     */
    void SetUp() override {
      logging::log_flush();
    }
  };

  #ifdef SUNSHINE_BUILD_DRM
  /**
   * @brief Capture method choice with KMS selected and the KMS display check replaced.
   *
   * Restores the configured capture method, the real KMS check, and an unchosen capture method afterward.
   */
  class CaptureSources: public testing::Test {
  protected:
    /**
     * @brief Select KMS capture and start with no capture method chosen.
     */
    void SetUp() override {
      saved_capture_ = config::video.capture;
      config::video.capture = "kms";
      platf::reset_capture_sources_for_testing();
    }

    /**
     * @brief Restore the real KMS check, the configured capture method, and an unchosen capture method.
     */
    void TearDown() override {
      platf::set_kms_verifier_for_testing({});
      platf::reset_capture_sources_for_testing();
      config::video.capture = saved_capture_;
    }

  private:
    std::string saved_capture_;  ///< Capture method configured before the test.
  };
  #endif
}  // namespace

TEST_F(ProcessEnvironmentSecurity, RemovesModuleLoaders) {
  std::array<std::optional<environment_guard_t>, unsafe_environment_variables.size()> guards;
  for (std::size_t index = 0; index < unsafe_environment_variables.size(); ++index) {
    const auto *variable = unsafe_environment_variables[index];
    guards[index].emplace(variable);
    ASSERT_EQ(setenv(variable, "/tmp/untrusted-module", 1), 0);
  }

  ASSERT_TRUE(platf::sanitize_process_environment());
  for (const auto *variable : unsafe_environment_variables) {
    EXPECT_EQ(std::getenv(variable), nullptr) << variable;
  }
}

TEST_F(ProcessEnvironmentSecurity, PreservesTraySessionVariables) {
  environment_guard_t platform_theme_guard {"QT_QPA_PLATFORMTHEME"};
  environment_guard_t wayland_display_guard {"WAYLAND_DISPLAY"};
  ASSERT_EQ(setenv("QT_QPA_PLATFORMTHEME", "gtk3", 1), 0);
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-test", 1), 0);

  ASSERT_TRUE(platf::sanitize_process_environment());
  EXPECT_STREQ(std::getenv("QT_QPA_PLATFORMTHEME"), "gtk3");
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-test");
}

TEST(LinuxCaptureSessionHint, LogsOnlyWithoutSessionVariablesAndWithoutKms) {
  logging::log_flush();
  environment_guard_t wayland_guard {"WAYLAND_DISPLAY"};
  environment_guard_t display_guard {"DISPLAY"};
  const auto original_capture = config::video.capture;
  auto restore_capture = util::fail_guard([&]() {
    config::video.capture = original_capture;
  });

  ASSERT_EQ(unsetenv("WAYLAND_DISPLAY"), 0);
  ASSERT_EQ(unsetenv("DISPLAY"), 0);

  for (const char *capture : {"", "kwin", "kms"}) {
    config::video.capture = capture;
    EXPECT_EQ(platf::log_missing_graphical_session_hint(), config::video.capture != "kms") << capture;
  }
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Error: Neither WAYLAND_DISPLAY nor DISPLAY is set"));

  config::video.capture = "kwin";
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-test", 1), 0);
  EXPECT_FALSE(platf::log_missing_graphical_session_hint());

  ASSERT_EQ(unsetenv("WAYLAND_DISPLAY"), 0);
  ASSERT_EQ(setenv("DISPLAY", ":1", 1), 0);
  EXPECT_FALSE(platf::log_missing_graphical_session_hint());
}

  #ifdef SUNSHINE_BUILD_DRM
// No display lit when the capture method is chosen at startup (every screen blanked by power saving): once a display
// is lit, KMS is chosen instead of keeping no capture method for the life of the process.
TEST_F(CaptureSources, ChoosesKmsOnceADisplayIsLit) {
  platf::set_kms_verifier_for_testing([] {
    return false;
  });
  EXPECT_FALSE(platf::choose_kms_capture_if_none());
  EXPECT_FALSE(platf::kms_capture_chosen_for_testing());

  platf::set_kms_verifier_for_testing([] {
    return true;
  });
  EXPECT_TRUE(platf::choose_kms_capture_if_none());
  EXPECT_TRUE(platf::kms_capture_chosen_for_testing());
}

// Listing displays before a probe is what chooses KMS when nothing is chosen yet.
TEST_F(CaptureSources, DisplayNamesChoosesKmsWhenNoneIsChosen) {
  platf::set_kms_verifier_for_testing([] {
    return true;
  });
  platf::display_names(platf::mem_type_e::unknown);
  EXPECT_TRUE(platf::kms_capture_chosen_for_testing());
}

// Once chosen, the capture method is kept: the displays aren't checked again.
TEST_F(CaptureSources, KeepsTheChoiceOnceMade) {
  int checks = 0;
  platf::set_kms_verifier_for_testing([&checks] {
    ++checks;
    return true;
  });
  EXPECT_TRUE(platf::choose_kms_capture_if_none());
  EXPECT_TRUE(platf::choose_kms_capture_if_none());
  EXPECT_EQ(checks, 1);
}

// Another capture method configured: KMS is never chosen later, and the KMS check isn't even run.
TEST_F(CaptureSources, OnlyKmsOrAutomaticIsCheckedAgain) {
  config::video.capture = "x11";
  int checks = 0;
  platf::set_kms_verifier_for_testing([&checks] {
    ++checks;
    return true;
  });
  EXPECT_FALSE(platf::choose_kms_capture_if_none());
  EXPECT_FALSE(platf::kms_capture_chosen_for_testing());
  EXPECT_EQ(checks, 0);
}
  #endif

#endif  // __linux__
