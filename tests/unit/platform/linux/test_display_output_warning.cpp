/**
 * @file tests/unit/platform/linux/test_display_output_warning.cpp
 * @brief Tests for Linux capture output-name migration guidance.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>

  // local includes
  #include "src/platform/linux/display_output_warning.h"

TEST(DisplayOutputWarningTest, WarnsForUnmatchedNumericSettingWithEnumeratedOutputs) {
  const std::array<std::string, 2> names {"DP-1", "HDMI-A-1"};

  EXPECT_TRUE(platf::needs_output_name_migration_warning(names, "0"));
  EXPECT_TRUE(platf::needs_output_name_migration_warning(names, "01"));
}

TEST(DisplayOutputWarningTest, DoesNotWarnForConnectorNamesOrTheDefaultSetting) {
  const std::array<std::string, 2> names {"DP-1", "HDMI-A-1"};

  for (const auto name : {"DP-1", "DP-9", "", "1x"}) {
    EXPECT_FALSE(platf::needs_output_name_migration_warning(names, name));
  }
}

TEST(DisplayOutputWarningTest, PreservesNumericEntriesAndNoEnumerationFallback) {
  const std::array<std::string, 2> names {"DP-1", "1"};

  EXPECT_FALSE(platf::needs_output_name_migration_warning(names, "1"));
  EXPECT_FALSE(platf::needs_output_name_migration_warning({}, "1"));
}

#endif
