/**
 * @file tests/unit/platform/linux/test_display_output_selection.cpp
 * @brief Tests for legacy numeric Linux capture output selection.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>

  // local includes
  #include "src/platform/linux/display_output_selection.h"

TEST(KmsOutputSelectionTest, ParsesZeroBasedCaptureIndexes) {
  EXPECT_EQ(platf::display_output::parse_output_index("0"), 0);
  EXPECT_EQ(platf::display_output::parse_output_index("010"), 10);
}

TEST(KmsOutputSelectionTest, RejectsConnectorNamesAndInvalidNumbers) {
  for (const auto name : {"DP-1", "", "2x", "+1", "-1", " 1", "999999999999999999999999999999999999"}) {
    EXPECT_FALSE(platf::display_output::parse_output_index(name));
  }
}

TEST(KmsOutputSelectionTest, MatchesCaptureIndexWithoutCountingSkippedPlanesAsUsableDisplays) {
  std::optional<std::size_t> selected;
  const auto requested = platf::display_output::parse_output_index("2");

  platf::display_output::match_output_index(requested, 0, 0, selected);
  EXPECT_FALSE(selected);

  // Capture index 1 was skipped during enumeration.
  platf::display_output::match_output_index(requested, 2, 1, selected);
  EXPECT_EQ(selected, 1);

  platf::display_output::match_output_index(requested, 3, 2, selected);
  EXPECT_EQ(selected, 1);
}

TEST(KmsOutputSelectionTest, LeavesFallbackUnchangedForConnectorNamesOrUnusableIndexes) {
  std::optional<std::size_t> selected;

  for (const auto name : {"DP-1", "1", "99"}) {
    const auto requested = platf::display_output::parse_output_index(name);
    platf::display_output::match_output_index(requested, 0, 0, selected);
    platf::display_output::match_output_index(requested, 2, 1, selected);
    EXPECT_FALSE(selected);
  }
}

TEST(WlOutputSelectionTest, UsesRegistryIndexForLegacyConfig) {
  const std::array<std::string, 2> names {"DP-2", "HDMI-A-1"};

  EXPECT_EQ(platf::display_output::wl_output_index(names, "0"), 0);
  EXPECT_EQ(platf::display_output::wl_output_index(names, "1"), 1);
}

TEST(WlOutputSelectionTest, LeavesExactNameMatchingToSharedSelector) {
  const std::array<std::string, 2> names {"DP-2", "0"};

  EXPECT_FALSE(platf::display_output::wl_output_index(names, "DP-2"));
  EXPECT_FALSE(platf::display_output::wl_output_index(names, "0"));
}

TEST(WlOutputSelectionTest, LeavesFallbackUnchangedForInvalidInputs) {
  const std::array<std::string, 2> names {"DP-2", "HDMI-A-1"};

  for (const auto name : {"2", "DP-9", "", "1x", "-1"}) {
    EXPECT_FALSE(platf::display_output::wl_output_index(names, name));
  }
  EXPECT_FALSE(platf::display_output::wl_output_index({}, "0"));
}

#endif
