/**
 * @file tests/unit/test_config.cpp
 * @brief Tests for Sunshine configuration parsing, range validation, and persistence helpers.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

// local includes
#include <src/config.h>
#include <src/file_handler.h>
#include <src/utility.h>

using namespace std::literals;

/**
 * @brief Preparation commands own their command strings and copy privilege settings.
 */
TEST(ConfigPrepCommandTest, PreservesCommandsAndElevationWithBothConstructors) {
  const config::prep_cmd_t paired {"launch"s, "undo"s, true};
  EXPECT_EQ(paired.do_cmd, "launch");
  EXPECT_EQ(paired.undo_cmd, "undo");
  EXPECT_TRUE(paired.elevated);

  const config::prep_cmd_t single {"launch-only"s, false};
  EXPECT_EQ(single.do_cmd, "launch-only");
  EXPECT_TRUE(single.undo_cmd.empty());
  EXPECT_FALSE(single.elevated);
}

TEST(ConfigParsingTest, ParsedEntriesOwnKeysAndSupportStringViewLookup) {
  std::string input = "encoder = software\nencoder = nvenc\n";
  const auto entries = config::parse_config(input);
  input.assign(input.size(), 'x');

  const std::string padded = "__encoder__";
  const auto key = std::string_view {padded}.substr(2, 7);
  const auto found = entries.find(key);
  ASSERT_NE(found, entries.end());
  EXPECT_EQ(found->second, "software");
  EXPECT_FALSE(entries.contains("missing"sv));
  EXPECT_EQ(entries.size(), 1);
}

/**
 * @brief Duplicate configuration keys retain their first nonempty entry.
 */
TEST(ConfigParsingTest, DuplicateKeysPreserveFirstValueAfterIgnoredEmptyEntry) {
  const auto entries = config::parse_config("encoder = software\nencoder = nvenc\noutput_name =\noutput_name = later\noutput_name = ignored\n");
  EXPECT_EQ(entries.at("encoder"), "software");
  EXPECT_EQ(entries.at("output_name"), "later");
  EXPECT_EQ(entries.size(), 2);
}

TEST(ConfigDefaultsTest, UsesAutomaticGamepadSelection) {
  EXPECT_EQ(config::input.gamepad, "auto");
}

TEST(ConfigDefaultsTest, TreatsFp16SdrCapturesAsLinear) {
  EXPECT_FALSE(config::video.fp16_sdr_gamma_encoded);
}

TEST(ConfigDefaultsTest, PreservesAmdEncoderNumericDefaults) {
  EXPECT_EQ(config::video.amd.amd_usage_h264, 1);
  EXPECT_EQ(config::video.amd.amd_usage_hevc, 1);
  EXPECT_EQ(config::video.amd.amd_usage_av1, 2);
  EXPECT_EQ(config::video.amd.amd_rc_h264, 3);
  EXPECT_EQ(config::video.amd.amd_rc_hevc, 1);
  EXPECT_EQ(config::video.amd.amd_rc_av1, 1);
  EXPECT_EQ(config::video.amd.amd_quality_h264, 0);
  EXPECT_EQ(config::video.amd.amd_quality_hevc, 5);
  EXPECT_EQ(config::video.amd.amd_quality_av1, 70);
  EXPECT_EQ(config::video.amd.amd_coder, 0);
}

using NvencPresetNameParam = std::pair<int, std::string_view>;

/**
 * @brief Parameterized coverage for FFmpeg's symbolic NVENC preset names.
 */
struct NvencPresetNameTest: testing::TestWithParam<NvencPresetNameParam> {};

TEST_P(NvencPresetNameTest, UsesStableSymbolicName) {
  const auto &[quality_preset, expected] = GetParam();

  EXPECT_EQ(expected, config::nv::ffmpeg_preset_from_quality(quality_preset));
}

INSTANTIATE_TEST_SUITE_P(
  NvencQualityPresets,
  NvencPresetNameTest,
  testing::Values(
    NvencPresetNameParam {1, "p1"sv},
    NvencPresetNameParam {2, "p2"sv},
    NvencPresetNameParam {3, "p3"sv},
    NvencPresetNameParam {4, "p4"sv},
    NvencPresetNameParam {5, "p5"sv},
    NvencPresetNameParam {6, "p6"sv},
    NvencPresetNameParam {7, "p7"sv}
  )
);

namespace {

  /**
   * @brief Fixture that redirects configuration persistence to a temporary file.
   */
  class ConfigPersistenceTest: public testing::Test {
  protected:
    /**
     * @brief Preserve the active config path and prepare an empty temporary path.
     */
    void SetUp() override {
      original_config_file_ = config::sunshine.config_file;
      original_gamepad_driver_ = config::input.gamepad_driver;
      original_gamepad_ = config::input.gamepad;
      original_apps_file_ = config::stream.file_apps;
      config_file_ = std::filesystem::temp_directory_path() / "sunshine_test_config_persistence.conf";  // NOSONAR(cpp:S5443): safe for tests
      std::filesystem::remove(config_file_);
      config::sunshine.config_file = config_file_.string();
      config::input.gamepad_driver.clear();
    }

    /**
     * @brief Remove the temporary file and restore the active config path.
     */
    void TearDown() override {
      config::sunshine.config_file = std::move(original_config_file_);
      config::input.gamepad_driver = std::move(original_gamepad_driver_);
      config::input.gamepad = std::move(original_gamepad_);
      config::stream.file_apps = std::move(original_apps_file_);
      std::filesystem::remove(config_file_.parent_path() / "sunshine_test_config_apps.json");
      std::filesystem::remove(config_file_);
    }

    /**
     * @brief Get the temporary configuration file used by the test.
     *
     * @return Temporary configuration file path.
     */
    const std::filesystem::path &config_file() const {
      return config_file_;
    }

  private:
    std::filesystem::path config_file_;  ///< Temporary configuration file used by the test.
    std::string original_config_file_;  ///< Active configuration path restored after each test.
    std::string original_gamepad_driver_;  ///< Gamepad driver preference restored after each test.
    std::string original_gamepad_;  ///< Gamepad profile restored after each test.
    std::string original_apps_file_;  ///< Applications file path restored after each test.
  };

}  // namespace

TEST_F(ConfigPersistenceTest, AppendsMissingOptionWithoutReplacingExistingText) {
  ASSERT_EQ(file_handler::write_file(config_file().string().c_str(), "# retained comment"), 0);

  EXPECT_TRUE(config::persist_config_option_if_missing("gamepad_driver", "all"));
  EXPECT_EQ(
    file_handler::read_file(config_file().string().c_str()),
    "# retained comment\ngamepad_driver = all\n"
  );

  EXPECT_FALSE(config::persist_config_option_if_missing("gamepad_driver", "virtualhid"));
  EXPECT_EQ(
    file_handler::read_file(config_file().string().c_str()),
    "# retained comment\ngamepad_driver = all\n"
  );
}

TEST_F(ConfigPersistenceTest, DoesNotReplaceAnExistingOption) {
  ASSERT_EQ(file_handler::write_file(config_file().string().c_str(), "gamepad_driver = vigembus\n"), 0);

  EXPECT_FALSE(config::persist_config_option_if_missing("gamepad_driver", "all"));
  EXPECT_EQ(file_handler::read_file(config_file().string().c_str()), "gamepad_driver = vigembus\n");
}

TEST_F(ConfigPersistenceTest, SelectsAllDriversOnlyForLicensedUsersWithoutAPreference) {
  EXPECT_FALSE(config::select_all_gamepad_drivers_if_licensed(false));
  EXPECT_TRUE(config::input.gamepad_driver.empty());
  EXPECT_TRUE(file_handler::read_file(config_file().string().c_str()).empty());

  EXPECT_TRUE(config::select_all_gamepad_drivers_if_licensed(true));
  EXPECT_EQ(config::input.gamepad_driver, config::GAMEPAD_DRIVER_ALL);
  EXPECT_EQ(file_handler::read_file(config_file().string().c_str()), "gamepad_driver = all\n");

  EXPECT_FALSE(config::select_all_gamepad_drivers_if_licensed(true));
  EXPECT_EQ(file_handler::read_file(config_file().string().c_str()), "gamepad_driver = all\n");
}

TEST_F(ConfigPersistenceTest, AcceptsAllGamepadProfilesAcrossRepeatedConfigurationChanges) {
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  constexpr std::array profiles {"generic"sv, "x360"sv, "xone"sv, "xseries"sv, "ds4"sv, "ds5"sv, "switch"sv};
  for (const auto profile : profiles) {
    config::input.gamepad = "auto";
    config::apply_config_for_test(std::string {"gamepad = "} + std::string {profile} + "\n");
    EXPECT_EQ(config::input.gamepad, profile);
  }
}

TEST_F(ConfigPersistenceTest, ParsesFp16SdrGammaEncodedOption) {
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  config::apply_config_for_test("fp16_sdr_gamma_encoded = enabled\n");
  EXPECT_TRUE(config::video.fp16_sdr_gamma_encoded);

  config::apply_config_for_test("fp16_sdr_gamma_encoded = disabled\n");
  EXPECT_FALSE(config::video.fp16_sdr_gamma_encoded);
}

/**
 * @brief Verify the DSCP tagging option parses enabled and disabled values.
 */
TEST_F(ConfigPersistenceTest, ParsesDscpTaggingOption) {
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  config::apply_config_for_test("dscp_tagging = disabled\n");
  EXPECT_FALSE(config::stream.dscp_tagging);

  config::apply_config_for_test("dscp_tagging = enabled\n");
  EXPECT_TRUE(config::stream.dscp_tagging);
}

/**
 * @brief Verify mixed-case boolean values update configuration correctly.
 */
TEST_F(ConfigPersistenceTest, ParsesBooleanOptionsCaseInsensitively) {
  const auto original_video = config::video;
  const auto restore_video = util::fail_guard([original_video]() {
    config::video = original_video;
  });
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  constexpr std::array cases {
    std::pair {"Enabled"sv, true},
    std::pair {"TRUE"sv, true},
    std::pair {"On"sv, true},
    std::pair {"Yes"sv, true},
    std::pair {"Disabled"sv, false},
    std::pair {"False"sv, false},
    std::pair {"OFF"sv, false},
  };
  for (const auto &[setting, expected] : cases) {
    config::video.fp16_sdr_gamma_encoded = !expected;
    config::apply_config_for_test(std::string {"fp16_sdr_gamma_encoded = "} + std::string {setting} + "\n");
    EXPECT_EQ(config::video.fp16_sdr_gamma_encoded, expected) << setting;
  }
}

TEST_F(ConfigPersistenceTest, ParsesHexIntegersWiderThanAnInt) {
  const auto original_video = config::video;
  const auto restore_video = util::fail_guard([original_video]() {
    config::video = original_video;
  });
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  config::apply_config_for_test("min_threads = 0x1234567890\n");
  EXPECT_EQ(config::video.min_threads, 0x34567890);
}

TEST_F(ConfigPersistenceTest, ParsesFecPercentagesAndRejectsOutOfRangeValues) {
  const auto original_stream = config::stream;
  const auto restore_stream = util::fail_guard([original_stream]() {
    config::stream = original_stream;
  });
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  constexpr std::array cases {
    std::pair {0, 0},
    std::pair {1, 1},
    std::pair {20, 20},
    std::pair {99, 99},
    std::pair {100, 100},
    std::pair {-1, 20},
    std::pair {101, 20},
    std::pair {255, 20},
  };
  for (const auto &[setting, expected] : cases) {
    config::stream.fec_percentage = 20;
    config::apply_config_for_test(std::format("fec_percentage = {}\n", setting));
    EXPECT_EQ(config::stream.fec_percentage, expected) << setting;
  }
}

TEST_F(ConfigPersistenceTest, ParsesNvencTwoPassModesAndDefaultsUnknownValues) {
  using enum nvenc::nvenc_two_pass;
  const auto original_video = config::video;
  const auto restore_video = util::fail_guard([original_video]() {
    config::video = original_video;
  });
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  constexpr std::array cases {
    std::pair {"disabled"sv, disabled},
    std::pair {"quarter_res"sv, quarter_resolution},
    std::pair {"full_res"sv, full_resolution},
    std::pair {"unknown"sv, quarter_resolution},
  };
  for (const auto &[setting, expected] : cases) {
    config::video.nv.two_pass = full_resolution;
    config::apply_config_for_test(std::string {"nvenc_twopass = "} + std::string {setting} + "\n");
    EXPECT_EQ(config::video.nv.two_pass, expected) << setting;
  }
}

TEST_F(ConfigPersistenceTest, ParsesMaxStreamResolutionAndRejectsMalformedValues) {
  const auto original_video = config::video;
  const auto restore_video = util::fail_guard([original_video]() {
    config::video = original_video;
  });
  const auto apps_file = config_file().parent_path() / "sunshine_test_config_apps.json";
  ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
  config::stream.file_apps = apps_file.string();

  config::video.max_stream_width = 0;
  config::video.max_stream_height = 0;
  config::apply_config_for_test("max_stream_resolution = 2560x1440\n");
  EXPECT_EQ(config::video.max_stream_width, 2560);
  EXPECT_EQ(config::video.max_stream_height, 1440);

  constexpr std::array malformed {
    "2560"sv,
    "2560x"sv,
    "x1440"sv,
    "0x1440"sv,
    "2560x0"sv,
    "-2560x1440"sv,
    "2560x1440x"sv,
    "2560 x 1440"sv,
    "widthxheight"sv,
  };
  for (const auto setting : malformed) {
    config::video.max_stream_width = 1;
    config::video.max_stream_height = 1;
    config::apply_config_for_test(std::string {"max_stream_resolution = "} + std::string {setting} + "\n");
    EXPECT_EQ(config::video.max_stream_width, 1) << setting;
    EXPECT_EQ(config::video.max_stream_height, 1) << setting;
  }
}

/**
 * @brief Expected outcome of capping one client-requested resolution.
 */
struct StreamResolutionCapCase {
  int max_width;  ///< Configured maximum width.
  int max_height;  ///< Configured maximum height.
  int width;  ///< Client-requested width.
  int height;  ///< Client-requested height.
  bool capped;  ///< Whether the cap applies.
  int expected_width;  ///< Width after capping.
  int expected_height;  ///< Height after capping.
};

/**
 * @brief Parameterized fixture for config::cap_stream_resolution.
 */
struct StreamResolutionCapTest: testing::TestWithParam<StreamResolutionCapCase> {};

TEST_P(StreamResolutionCapTest, CapsPreservingAspectRatioWithEvenDimensions) {
  const auto original_video = config::video;
  const auto restore_video = util::fail_guard([original_video]() {
    config::video = original_video;
  });
  const auto &param = GetParam();
  config::video.max_stream_width = param.max_width;
  config::video.max_stream_height = param.max_height;

  int width = param.width;
  int height = param.height;
  EXPECT_EQ(config::cap_stream_resolution(width, height), param.capped);
  EXPECT_EQ(width, param.expected_width);
  EXPECT_EQ(height, param.expected_height);
}

INSTANTIATE_TEST_SUITE_P(
  ConfigCapStreamResolution,
  StreamResolutionCapTest,
  testing::Values(
    // Disabled cap
    StreamResolutionCapCase {0, 0, 3840, 2160, false, 3840, 2160},
    StreamResolutionCapCase {2560, 0, 3840, 2160, false, 3840, 2160},
    // Requests within the cap are untouched
    StreamResolutionCapCase {2560, 1440, 2560, 1440, false, 2560, 1440},
    StreamResolutionCapCase {2560, 1440, 1920, 1080, false, 1920, 1080},
    // Invalid requests are untouched
    StreamResolutionCapCase {2560, 1440, 0, 2160, false, 0, 2160},
    // Same aspect ratio lands exactly on the cap
    StreamResolutionCapCase {2560, 1440, 3840, 2160, true, 2560, 1440},
    // Width-limited ultrawide: 1440 * 2560 / 3440 = 1071.6, rounded down to even
    StreamResolutionCapCase {2560, 1440, 3440, 1440, true, 2560, 1070},
    // Height-limited portrait request
    StreamResolutionCapCase {2560, 1440, 2160, 3840, true, 810, 1440},
    // A single dimension over the cap still scales both
    StreamResolutionCapCase {1920, 1080, 1920, 1200, true, 1728, 1080},
    // Floating-point scaling used to land two pixels short of the cap here
    StreamResolutionCapCase {1280, 720, 640, 1142, true, 402, 720},
    // Extreme aspect ratios never collapse to zero
    StreamResolutionCapCase {100, 100, 10000, 2, true, 100, 2}
  )
);
