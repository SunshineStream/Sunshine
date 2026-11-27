/**
 * @file tests/unit/platform/macos/test_display_uuid.cpp
 * @brief Tests for macOS display UUID lookup.
 */

#ifdef __APPLE__
  // standard includes
  #include <algorithm>
  #include <cctype>

  // platform includes
  #include <CoreGraphics/CoreGraphics.h>

  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/platform/macos/misc.h"

TEST(MacosDisplayUuidTest, UnknownUuidHasNoDisplay) {
  EXPECT_FALSE(platf::display_id_from_uuid("").has_value());
  EXPECT_FALSE(platf::display_id_from_uuid("3").has_value());
  EXPECT_FALSE(platf::display_id_from_uuid("00000000-0000-0000-0000-000000000000").has_value());
}

TEST(MacosDisplayUuidTest, OnlineDisplayUuidRoundTrips) {
  std::uint32_t display_count {};
  CGDirectDisplayID display_id {};
  if (CGGetOnlineDisplayList(1, &display_id, &display_count) != kCGErrorSuccess || display_count == 0) {
    GTEST_SKIP() << "No online display available";
  }

  const auto uuid {platf::display_uuid(display_id)};
  ASSERT_EQ(uuid.size(), 36U);
  EXPECT_EQ(platf::display_id_from_uuid(uuid), display_id);

  std::string lower_case_uuid {uuid};
  std::ranges::transform(lower_case_uuid, std::begin(lower_case_uuid), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  EXPECT_EQ(platf::display_id_from_uuid(lower_case_uuid), display_id);
}
#endif
