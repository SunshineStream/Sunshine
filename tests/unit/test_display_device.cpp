/**
 * @file tests/unit/test_display_device.cpp
 * @brief Test src/display_device.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

// lib includes
#include <display_device/settings_manager_interface.h>
#include <display_device/settings_persistence_interface.h>

// local includes
#include <src/config.h>
#include <src/display_device.h>
#include <src/rtsp.h>

namespace {
  using config_option_e = config::video_t::dd_t::config_option_e;
  using device_prep_t = display_device::SingleDisplayConfiguration::DevicePreparation;

  using hdr_option_e = config::video_t::dd_t::hdr_option_e;
  using hdr_state_e = display_device::HdrState;

  using resolution_option_e = config::video_t::dd_t::resolution_option_e;
  using resolution_t = display_device::Resolution;

  using refresh_rate_option_e = config::video_t::dd_t::refresh_rate_option_e;
  using rational_t = display_device::Rational;

  struct failed_to_parse_resolution_tag_t {};

  struct failed_to_parse_refresh_rate_tag_t {};

  struct no_refresh_rate_tag_t {};

  struct no_resolution_tag_t {};

  struct client_resolution_t {
    int width;
    int height;
  };

  using client_fps_t = int;
  using sops_enabled_t = bool;
  using client_wants_hdr_t = bool;

  constexpr unsigned int max_uint {std::numeric_limits<unsigned int>::max()};
  const std::string max_uint_string {std::to_string(std::numeric_limits<unsigned int>::max())};

  template<class T>
  struct DisplayDeviceConfigTest: BaseTest, testing::WithParamInterface<T> {};
}  // namespace

using ParseDeviceId = DisplayDeviceConfigTest<std::pair<std::string, std::string>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseDeviceId,
  testing::Values(
    std::make_pair(""s, ""s),
    std::make_pair("SomeId"s, "SomeId"s),
    std::make_pair("{daeac860-f4db-5208-b1f5-cf59444fb768}"s, "{daeac860-f4db-5208-b1f5-cf59444fb768}"s)
  )
);

TEST_P(ParseDeviceId, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.output_name = input_value;

  const auto result {display_device::parse_configuration(video_config, {})};
  EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_device_id, expected_value);
}

using ParseConfigOption = DisplayDeviceConfigTest<std::pair<config_option_e, std::optional<device_prep_t>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseConfigOption,
  testing::Values(
    std::make_pair(config_option_e::disabled, std::nullopt),
    std::make_pair(config_option_e::verify_only, device_prep_t::VerifyOnly),
    std::make_pair(config_option_e::ensure_active, device_prep_t::EnsureActive),
    std::make_pair(config_option_e::ensure_primary, device_prep_t::EnsurePrimary),
    std::make_pair(config_option_e::ensure_only_display, device_prep_t::EnsureOnlyDisplay)
  )
);

TEST_P(ParseConfigOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();

  config::video_t video_config {};
  video_config.dd.configuration_option = input_value;

  const auto result {display_device::parse_configuration(video_config, {})};
  if (const auto *parsed_config {std::get_if<display_device::SingleDisplayConfiguration>(&result)}; parsed_config) {
    ASSERT_EQ(parsed_config->m_device_prep, expected_value);
  } else {
    ASSERT_EQ(std::get_if<display_device::configuration_disabled_tag_t>(&result) != nullptr, !expected_value);
  }
}

using ParseHdrOption = DisplayDeviceConfigTest<std::pair<std::pair<hdr_option_e, client_wants_hdr_t>, std::optional<hdr_state_e>>>;
/**
 * @brief HDR parsing cases, including the platform-specific automatic policy.
 */
const auto hdr_option_cases = std::to_array<ParseHdrOption::ParamType>({std::make_pair(std::make_pair(hdr_option_e::disabled, client_wants_hdr_t {true}), std::nullopt), std::make_pair(std::make_pair(hdr_option_e::disabled, client_wants_hdr_t {false}), std::nullopt),
#ifdef __APPLE__
                                                                        std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {true}), std::nullopt),
                                                                        std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {false}), std::nullopt)
#else
                                                                        std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {true}), hdr_state_e::Enabled),
                                                                        std::make_pair(std::make_pair(hdr_option_e::automatic, client_wants_hdr_t {false}), hdr_state_e::Disabled)
#endif
});

INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseHdrOption,
  testing::ValuesIn(hdr_option_cases)
);

TEST_P(ParseHdrOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();
  const auto &[input_hdr_option, input_enable_hdr] = input_value;

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.dd.hdr_option = input_hdr_option;

  rtsp_stream::launch_session_t session {};
  session.enable_hdr = input_enable_hdr;

  const auto result {display_device::parse_configuration(video_config, session)};
  EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_hdr_state, expected_value);
}

using ParseResolutionOption = DisplayDeviceConfigTest<std::pair<std::tuple<resolution_option_e, sops_enabled_t, std::variant<client_resolution_t, std::string>>, std::variant<failed_to_parse_resolution_tag_t, no_resolution_tag_t, resolution_t>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseResolutionOption,
  testing::Values(
    //---- Disabled cases ----
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {true}, "invalid_res"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::disabled, sops_enabled_t {false}, "invalid_res"s), no_resolution_tag_t {}),
    //---- Automatic cases ----
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {1920, 1080}), resolution_t {1920, 1080}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, "1920x1080"s), resolution_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {-1, -1}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, "invalid_res"s), resolution_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {false}, "invalid_res"s), no_resolution_tag_t {}),
    //---- Manual cases ----
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, client_resolution_t {1920, 1080}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "1920x1080"s), resolution_t {1920, 1080}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, client_resolution_t {-1, -1}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "invalid_res"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, client_resolution_t {1920, 1080}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, "1920x1080"s), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, client_resolution_t {-1, -1}), no_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {false}, "invalid_res"s), no_resolution_tag_t {}),
    //---- Both negative values from client are checked ----
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {0, 0}), resolution_t {0, 0}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {-1, 0}), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::automatic, sops_enabled_t {true}, client_resolution_t {0, -1}), failed_to_parse_resolution_tag_t {}),
    //---- Resolution string format validation ----
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "0x0"s), resolution_t {0, 0}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "0x"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "x0"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "-1x1"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "1x-1"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "x0x0"s), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, "0x0x"s), failed_to_parse_resolution_tag_t {}),
    //---- String number is out of bounds ----
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, max_uint_string + "x"s + max_uint_string), resolution_t {max_uint, max_uint}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, max_uint_string + "0"s + "x"s + max_uint_string), failed_to_parse_resolution_tag_t {}),
    std::make_pair(std::make_tuple(resolution_option_e::manual, sops_enabled_t {true}, max_uint_string + "x"s + max_uint_string + "0"s), failed_to_parse_resolution_tag_t {})
  )
);

TEST_P(ParseResolutionOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();
  const auto &[input_resolution_option, input_enable_sops, input_resolution] = input_value;

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.dd.resolution_option = input_resolution_option;

  rtsp_stream::launch_session_t session {};
  session.enable_sops = input_enable_sops;

  if (const auto *client_res {std::get_if<client_resolution_t>(&input_resolution)}; client_res) {
    video_config.dd.manual_resolution = {};
    session.width = client_res->width;
    session.height = client_res->height;
  } else {
    video_config.dd.manual_resolution = std::get<std::string>(input_resolution);
    session.width = {};
    session.height = {};
  }

  const auto result {display_device::parse_configuration(video_config, session)};
  if (const auto *failed_option {std::get_if<failed_to_parse_resolution_tag_t>(&expected_value)}; failed_option) {
    EXPECT_NO_THROW((void) std::get<display_device::failed_to_parse_tag_t>(result));
  } else {
    std::optional<resolution_t> expected_resolution;
    if (const auto *valid_resolution_option {std::get_if<resolution_t>(&expected_value)}; valid_resolution_option) {
      expected_resolution = *valid_resolution_option;
    }

    EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_resolution, expected_resolution);
  }
}

using ParseRefreshRateOption = DisplayDeviceConfigTest<std::pair<std::tuple<refresh_rate_option_e, std::variant<client_fps_t, std::string>>, std::variant<failed_to_parse_refresh_rate_tag_t, no_refresh_rate_tag_t, rational_t>>>;
INSTANTIATE_TEST_SUITE_P(
  DisplayDeviceConfigTest,
  ParseRefreshRateOption,
  testing::Values(
    //---- Disabled cases ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, client_fps_t {60}), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, "60"s), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, "59.9885"s), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, client_fps_t {-1}), no_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::disabled, "invalid_refresh_rate"s), no_refresh_rate_tag_t {}),
    //---- Automatic cases ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, client_fps_t {60}), rational_t {60, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, "60"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, "59.9885"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, client_fps_t {-1}), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::automatic, "invalid_refresh_rate"s), rational_t {0, 1}),
    //---- Manual cases ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, client_fps_t {60}), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60"s), rational_t {60, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "59.9885"s), rational_t {599885, 10000}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, client_fps_t {-1}), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "invalid_refresh_rate"s), failed_to_parse_refresh_rate_tag_t {}),
    //---- Refresh rate string format validation ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "0000000000000"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "0"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000000.0000000"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "0.0"s), rational_t {0, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "000000000000010"s), rational_t {10, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000010.0000000"s), rational_t {10, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000010.1000000"s), rational_t {101, 10}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000010.0100000"s), rational_t {1001, 100}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "00000000.1000000"s), rational_t {1, 10}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60,0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "-60.0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60.-0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "a60.0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60.0b"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "a60"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "60b"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, "-60"s), failed_to_parse_refresh_rate_tag_t {}),
    //---- String number is out of bounds ----
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string), rational_t {max_uint, 1}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string + "0"s), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string.substr(0, 1) + "."s + max_uint_string.substr(1)), rational_t {max_uint, static_cast<unsigned int>(std::pow(10, max_uint_string.size() - 1))}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string.substr(0, 1) + "0"s + "."s + max_uint_string.substr(1)), failed_to_parse_refresh_rate_tag_t {}),
    std::make_pair(std::make_tuple(refresh_rate_option_e::manual, max_uint_string.substr(0, 1) + "."s + "0"s + max_uint_string.substr(1)), failed_to_parse_refresh_rate_tag_t {})
  )
);

TEST_P(ParseRefreshRateOption, IntegrationTest) {
  const auto &[input_value, expected_value] = GetParam();
  const auto &[input_refresh_rate_option, input_refresh_rate] = input_value;

  config::video_t video_config {};
  video_config.dd.configuration_option = config_option_e::verify_only;
  video_config.dd.refresh_rate_option = input_refresh_rate_option;

  rtsp_stream::launch_session_t session {};
  if (const auto *client_refresh_rate {std::get_if<client_fps_t>(&input_refresh_rate)}; client_refresh_rate) {
    video_config.dd.manual_refresh_rate = {};
    session.fps = *client_refresh_rate;
  } else {
    video_config.dd.manual_refresh_rate = std::get<std::string>(input_refresh_rate);
    session.fps = {};
  }

  const auto result {display_device::parse_configuration(video_config, session)};
  if (const auto *failed_option {std::get_if<failed_to_parse_refresh_rate_tag_t>(&expected_value)}; failed_option) {
    EXPECT_NO_THROW((void) std::get<display_device::failed_to_parse_tag_t>(result));
  } else {
    std::optional<display_device::FloatingPoint> expected_refresh_rate;
    if (const auto *valid_refresh_rate_option {std::get_if<rational_t>(&expected_value)}; valid_refresh_rate_option) {
      expected_refresh_rate = *valid_refresh_rate_option;
    }

    EXPECT_EQ(std::get<display_device::SingleDisplayConfiguration>(result).m_refresh_rate, expected_refresh_rate);
  }
}

namespace {
  using res_t = resolution_t;
  using fps_t = client_fps_t;
  using remap_entries_t = config::video_t::dd_t::mode_remapping_t;

  struct no_value_t {};

  template<class T>
  struct auto_value_t {
    T value;
  };

  template<class T>
  struct manual_value_t {
    T value;
  };

  using resolution_variant_t = std::variant<no_value_t, auto_value_t<res_t>, manual_value_t<res_t>>;
  using rational_variant_t = std::variant<no_value_t, auto_value_t<fps_t>, manual_value_t<fps_t>>;

  struct failed_to_remap_t {};

  struct final_values_t {
    std::optional<resolution_t> resolution;
    std::optional<rational_t> refresh_rate;
  };

  const std::string INVALID_RES {"INVALID"};
  const std::string INVALID_FPS {"1.23"};
  const std::string INVALID_REFRESH_RATE {"INVALID"};
  const remap_entries_t VALID_ENTRIES {
    .mixed = {
      {"1920x1080", "11", "1024x720", "1.11"},
      {"1920x1080", "", "1024x720", "2"},
      {"", "33", "1024x720", "3"},
      {"1920x720", "44", "1024x720", ""},
      {"1920x720", "55", "", "5"},
      {"1920x720", "", "1024x720", ""},
      {"", "11", "", "7.77"}
    },
    .resolution_only = {{"1920x1080", "", "720x720", ""}, {"1024x720", "", "1920x1920", ""}},
    .refresh_rate_only = {{"", "11", "", "1.23"}, {"", "22", "", "2.34"}}
  };
  const remap_entries_t INVALID_REQ_RES {
    .mixed = {{INVALID_RES, "11", "1024x720", "1.11"}},
    .resolution_only = {{INVALID_RES, "", "720x720", ""}},
    .refresh_rate_only = {{INVALID_RES, "11", "", "1.23"}}
  };
  const remap_entries_t INVALID_REQ_FPS {
    .mixed = {{"1920x1080", INVALID_FPS, "1024x720", "1.11"}},
    .resolution_only = {{"1920x1080", INVALID_FPS, "720x720", ""}},
    .refresh_rate_only = {{"", INVALID_FPS, "", "1.23"}}
  };
  const remap_entries_t INVALID_FINAL_RES {
    .mixed = {{"1920x1080", "11", INVALID_RES, "1.11"}},
    .resolution_only = {{"1920x1080", "", INVALID_RES, ""}},
    .refresh_rate_only = {{"", "11", INVALID_RES, "1.23"}}
  };
  const remap_entries_t INVALID_FINAL_REFRESH_RATE {
    .mixed = {{"1920x1080", "11", "1024x720", INVALID_REFRESH_RATE}},
    .resolution_only = {{"1920x1080", "", "720x720", INVALID_REFRESH_RATE}},
    .refresh_rate_only = {{"", "11", "", INVALID_REFRESH_RATE}}
  };
  const remap_entries_t EMPTY_REQ_ENTRIES {
    .mixed = {{"", "", "1024x720", "1.11"}},
    .resolution_only = {{"", "", "720x720", ""}},
    .refresh_rate_only = {{"", "", "", "1.23"}}
  };
  const remap_entries_t EMPTY_FINAL_ENTRIES {
    .mixed = {{"1920x1080", "11", "", ""}},
    .resolution_only = {{"1920x1080", "", "", ""}},
    .refresh_rate_only = {{"", "11", "", ""}}
  };

  using DisplayModeRemapping = DisplayDeviceConfigTest<std::pair<std::tuple<resolution_variant_t, rational_variant_t, sops_enabled_t, remap_entries_t>, std::variant<failed_to_remap_t, final_values_t>>>;
  INSTANTIATE_TEST_SUITE_P(
    DisplayDeviceConfigTest,
    DisplayModeRemapping,
    testing::Values(
      //---- Mixed (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{111, 100}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {120}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{2, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {33}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{3, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {44}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{44, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {55}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 720}}, {{5, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {60}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1024, 720}}, {{60, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {123}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1, 1}}, {{123, 1}}}),
      //---- Mixed (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{777, 100}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {120}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{120, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {33}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{33, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {44}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{44, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {55}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{55, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 720}, auto_value_t<fps_t> {60}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{60, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1, 1}, auto_value_t<fps_t> {123}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{123, 1}}}),
      //---- Resolution only (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1024, 720}, no_value_t {}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 1920}}, std::nullopt}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {11, 11}, manual_value_t<fps_t> {33}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{11, 11}}, {{33, 1}}}),
      //---- Resolution only (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1024, 720}, no_value_t {}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, std::nullopt}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {11, 11}, manual_value_t<fps_t> {33}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{33, 1}}}),
      //---- Refresh rate only (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      std::make_pair(std::make_tuple(no_value_t {}, auto_value_t<fps_t> {22}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {std::nullopt, {{234, 100}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {11, 11}, auto_value_t<fps_t> {33}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{11, 11}}, {{33, 1}}}),
      //---- Refresh rate only (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{123, 100}}}),
      std::make_pair(std::make_tuple(no_value_t {}, auto_value_t<fps_t> {22}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{234, 100}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {11, 11}, auto_value_t<fps_t> {33}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{33, 1}}}),
      //---- No mapping (valid), SOPS enabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {{{1920, 1080}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(no_value_t {}, no_value_t {}, sops_enabled_t {true}, VALID_ENTRIES), final_values_t {std::nullopt, std::nullopt}),
      //---- No mapping (valid), SOPS disabled ----
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(no_value_t {}, no_value_t {}, sops_enabled_t {false}, VALID_ENTRIES), final_values_t {std::nullopt, std::nullopt}),
      // ---- Invalid requested resolution, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_RES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      // ---- Invalid requested resolution, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_RES), final_values_t {std::nullopt, {{123, 100}}}),
      // ---- Invalid requested FPS, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_FPS), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_FPS), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_REQ_FPS), failed_to_remap_t {}),
      // ---- Invalid requested FPS, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_FPS), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_FPS), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_REQ_FPS), failed_to_remap_t {}),
      // ---- Invalid final resolution, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_RES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      // ---- Invalid final resolution, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_RES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_RES), final_values_t {std::nullopt, {{123, 100}}}),
      // ---- Invalid final refresh rate, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_REFRESH_RATE), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      // ---- Invalid final refresh rate, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_REFRESH_RATE), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, INVALID_FINAL_REFRESH_RATE), failed_to_remap_t {}),
      // ---- Empty req entries, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_REQ_ENTRIES), final_values_t {{{1024, 720}}, {{111, 100}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_REQ_ENTRIES), final_values_t {{{720, 720}}, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_REQ_ENTRIES), final_values_t {{{1920, 1080}}, {{123, 100}}}),
      // ---- Empty req entries, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_REQ_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_REQ_ENTRIES), final_values_t {std::nullopt, {{11, 1}}}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_REQ_ENTRIES), final_values_t {std::nullopt, {{123, 100}}}),
      // ---- Empty final entries, SOPS enabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {true}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      // ---- Empty final entries, SOPS disabled ----
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(auto_value_t<res_t> {1920, 1080}, manual_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {}),
      std::make_pair(std::make_tuple(manual_value_t<res_t> {1920, 1080}, auto_value_t<fps_t> {11}, sops_enabled_t {false}, EMPTY_FINAL_ENTRIES), failed_to_remap_t {})
    )
  );

  TEST_P(DisplayModeRemapping, IntegrationTest) {
    const auto &[input_value, expected_value] = GetParam();
    const auto &[input_res, input_fps, input_enable_sops, input_entries] = input_value;

    config::video_t video_config {};
    rtsp_stream::launch_session_t session {};

    {  // resolution
      using enum resolution_option_e;

      if (const auto *no_res {std::get_if<no_value_t>(&input_res)}; no_res) {
        video_config.dd.resolution_option = disabled;
      } else if (const auto *auto_res {std::get_if<auto_value_t<res_t>>(&input_res)}; auto_res) {
        video_config.dd.resolution_option = automatic;
        session.width = static_cast<int>(auto_res->value.m_width);
        session.height = static_cast<int>(auto_res->value.m_height);
      } else {
        const auto [manual_res] = std::get<manual_value_t<res_t>>(input_res);
        video_config.dd.resolution_option = manual;
        video_config.dd.manual_resolution = std::format("{}x{}", static_cast<int>(manual_res.m_width), static_cast<int>(manual_res.m_height));
      }
    }

    {  // fps
      using enum refresh_rate_option_e;

      if (const auto *no_fps {std::get_if<no_value_t>(&input_fps)}; no_fps) {
        video_config.dd.refresh_rate_option = disabled;
      } else if (const auto *auto_fps {std::get_if<auto_value_t<fps_t>>(&input_fps)}; auto_fps) {
        video_config.dd.refresh_rate_option = automatic;
        session.fps = auto_fps->value;
      } else {
        const auto [manual_fps] = std::get<manual_value_t<fps_t>>(input_fps);
        video_config.dd.refresh_rate_option = manual;
        video_config.dd.manual_refresh_rate = std::to_string(manual_fps);
      }
    }

    video_config.dd.configuration_option = config_option_e::verify_only;
    video_config.dd.mode_remapping = input_entries;
    session.enable_sops = input_enable_sops;

    const auto result {display_device::parse_configuration(video_config, session)};
    if (const auto *failed_option {std::get_if<failed_to_remap_t>(&expected_value)}; failed_option) {
      EXPECT_NO_THROW((void) std::get<display_device::failed_to_parse_tag_t>(result));
    } else {
      const auto &[expected_resolution, expected_refresh_rate] = std::get<final_values_t>(expected_value);
      const auto &parsed_config = std::get<display_device::SingleDisplayConfiguration>(result);

      EXPECT_EQ(parsed_config.m_resolution, expected_resolution);
      EXPECT_EQ(parsed_config.m_refresh_rate, expected_refresh_rate ? std::make_optional(display_device::FloatingPoint {*expected_refresh_rate}) : std::nullopt);
    }
  }
}  // namespace

namespace {
  /**
   * @brief Settings manager that reports success while a dock display's original settings are still saved.
   *
   * The first revert, while the dock is unplugged, stores the pending record and returns success.
   * Redocking makes the next revert clear that record, which is the library's signal that the restore finished.
   */
  class redock_settings_manager_t: public display_device::SettingsManagerInterface {
  public:
    /**
     * @brief Remember the persistence object the revert loop watches.
     * @param persistence Persistence object updated when a revert keeps or finishes pending settings.
     */
    explicit redock_settings_manager_t(std::shared_ptr<display_device::SettingsPersistenceInterface> persistence):
        persistence {std::move(persistence)} {}

    [[nodiscard]] display_device::EnumeratedDeviceList enumAvailableDevices() const override {
      std::lock_guard lock {mutex};
      return devices;
    }

    [[nodiscard]] std::string getDisplayName(const std::string &device_id) const override {
      return device_id;
    }

    [[nodiscard]] ApplyResult applySettings(const display_device::SingleDisplayConfiguration &config) override {
      std::lock_guard lock {mutex};
      ++apply_calls;
      last_applied_config = config;
      return apply_result;
    }

    [[nodiscard]] RevertResult revertSettings() override {
      std::lock_guard lock {mutex};
      ++revert_calls;
      if (keep_pending) {
        pending_saved = persistence->store({1});
        return RevertResult::Ok;
      }

      pending_saved = !persistence->clear();
      hdr_restored = !pending_saved;
      restored.notify_all();
      return RevertResult::Ok;
    }

    [[nodiscard]] bool resetPersistence() override {
      std::lock_guard lock {mutex};
      return persistence->clear();
    }

    /**
     * @brief Replace the connected displays.
     * @param next Displays visible to the next revert attempt.
     */
    void set_devices(display_device::EnumeratedDeviceList next) {
      std::lock_guard lock {mutex};
      devices = std::move(next);
    }

    /**
     * @brief Mark the dock as connected and ready for its saved settings to be restored.
     * @param next Displays visible once the dock is connected again.
     */
    void redock(display_device::EnumeratedDeviceList next) {
      std::lock_guard lock {mutex};
      keep_pending = false;
      devices = std::move(next);
    }

    /**
     * @brief Choose whether the next revert leaves the saved record in place.
     * @param pending True when a successful revert should keep the saved record.
     */
    void set_keep_pending(const bool pending) {
      std::lock_guard lock {mutex};
      keep_pending = pending;
    }

    /**
     * @brief Configure the result returned by `applySettings()`.
     * @param result Result enum to return on subsequent apply calls.
     */
    void set_apply_result(const ApplyResult result) {
      std::lock_guard lock {mutex};
      apply_result = result;
    }

    /**
     * @brief Report how many times `applySettings()` has been called.
     * @return Total apply count.
     */
    [[nodiscard]] int apply_call_count() const {
      std::lock_guard lock {mutex};
      return apply_calls;
    }

    /**
     * @brief Return the most recent configuration passed to `applySettings()`.
     * @return Last applied configuration, if any.
     */
    [[nodiscard]] std::optional<display_device::SingleDisplayConfiguration> latest_applied_config() const {
      std::lock_guard lock {mutex};
      return last_applied_config;
    }

    /**
     * @brief Revert counters observed by a test.
     */
    struct progress_t {
      int revert_calls {0};  ///< Number of revert attempts performed.
      bool pending_saved {false};  ///< True when the latest revert left a saved record.
      bool hdr_restored {false};  ///< True when redocking cleared the pending record.
    };

    /**
     * @brief Read the revert bookkeeping under the manager lock.
     * @return The latest revert counters.
     */
    [[nodiscard]] progress_t progress() const {
      std::lock_guard lock {mutex};
      return {revert_calls, pending_saved, hdr_restored};
    }

    /**
     * @brief Wait until redocking clears the pending record.
     * @param timeout Maximum time to wait.
     * @return True when the pending record was cleared before the timeout.
     */
    template<class Rep, class Period>
    [[nodiscard]] bool wait_for_hdr_restore(const std::chrono::duration<Rep, Period> &timeout) {
      std::unique_lock lock {mutex};
      return restored.wait_for(lock, timeout, [this]() {
        return hdr_restored;
      });
    }

  private:
    std::condition_variable restored;  ///< Wakes the test when the redocked display's settings are restored.
    std::shared_ptr<display_device::SettingsPersistenceInterface> persistence;  ///< Persistence observed by the revert loop.
    display_device::EnumeratedDeviceList devices {
      {.m_device_id = "panel", .m_friendly_name = "Panel"}
    };  ///< Displays currently connected.
    int revert_calls {0};  ///< Number of revert attempts performed.
    int apply_calls {0};  ///< Number of apply attempts performed.
    ApplyResult apply_result {ApplyResult::Ok};  ///< Result returned by `applySettings()`.
    std::optional<display_device::SingleDisplayConfiguration> last_applied_config {};  ///< Most recent configuration passed to `applySettings()`.
    bool keep_pending {true};  ///< When true, a successful revert still leaves the saved record in place.
    bool pending_saved {false};  ///< True when the latest revert left a saved record.
    bool hdr_restored {false};  ///< True when redocking cleared the pending record.
    mutable std::mutex mutex;  ///< Guards the device list and revert bookkeeping.
  };

  /**
   * @brief Stop the revert scheduler when a test ends.
   */
  class display_device_revert_guard_t {
  public:
    ~display_device_revert_guard_t() {
      display_device::test_reset_display_device();
    }
  };

  using namespace std::chrono_literals;

  /**
   * @brief Wait until the revert scheduler has stopped.
   * @return True when no retry remains scheduled.
   */
  bool wait_until_revert_stops() {
    for (int attempt {0}; attempt < 50 && display_device::test_revert_retry_is_scheduled(); ++attempt) {
      std::this_thread::sleep_for(5ms);
    }
    return !display_device::test_revert_retry_is_scheduled();
  }
}  // namespace

using namespace std::chrono_literals;

TEST(DisplayDeviceRevert, CompletedRevertStopsTheRetryLoop) {
  [[maybe_unused]] const display_device_revert_guard_t guard;
  display_device::test_set_retry_interval(30ms);
  const auto persistence {display_device::test_make_watched_persistence()};
  auto manager {std::make_unique<redock_settings_manager_t>(persistence)};
  manager->set_keep_pending(false);
  auto *manager_ptr {manager.get()};
  display_device::test_install_settings_manager(std::move(manager), persistence);

  display_device::revert_configuration();

  const auto completed {manager_ptr->progress()};
  EXPECT_EQ(completed.revert_calls, 1);
  EXPECT_TRUE(completed.hdr_restored);
  EXPECT_TRUE(wait_until_revert_stops());
}

TEST(DisplayDeviceRevert, RedockRestoresPendingSettings) {
  [[maybe_unused]] const display_device_revert_guard_t guard;
  display_device::test_set_retry_interval(30ms);
  const auto persistence {display_device::test_make_watched_persistence()};
  auto manager {std::make_unique<redock_settings_manager_t>(persistence)};
  auto *manager_ptr {manager.get()};
  display_device::test_install_settings_manager(std::move(manager), persistence);

  display_device::revert_configuration();

  EXPECT_TRUE(display_device::test_revert_retry_is_scheduled());
  const auto pending {manager_ptr->progress()};
  EXPECT_EQ(pending.revert_calls, 1);
  EXPECT_TRUE(pending.pending_saved);
  EXPECT_FALSE(pending.hdr_restored);

  // The dock is still absent, so later checks must not treat the pending recovery as finished.
  std::this_thread::sleep_for(80ms);
  EXPECT_EQ(manager_ptr->progress().revert_calls, 1);
  EXPECT_TRUE(display_device::test_revert_retry_is_scheduled());

  manager_ptr->redock({
    {.m_device_id = "panel", .m_friendly_name = "Panel"},
    {.m_device_id = "dock", .m_friendly_name = "Dock"},
  });

  ASSERT_TRUE(manager_ptr->wait_for_hdr_restore(2s));
  EXPECT_GE(manager_ptr->progress().revert_calls, 2);
  EXPECT_TRUE(wait_until_revert_stops());
}

TEST(DisplayDeviceReapply, SkipsWhenInactiveVerifyOnlyOrAfterRevert) {
  [[maybe_unused]] const display_device_revert_guard_t guard;
  const auto persistence {display_device::test_make_watched_persistence()};
  auto manager {std::make_unique<redock_settings_manager_t>(persistence)};
  manager->set_keep_pending(false);
  auto *manager_ptr {manager.get()};
  display_device::test_install_settings_manager(std::move(manager), persistence);

  // No active configuration yet.
  EXPECT_FALSE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 0);

  // VerifyOnly cannot activate a deactivated display.
  display_device::SingleDisplayConfiguration verify_config;
  verify_config.m_device_id = "{vdd}";
  verify_config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::VerifyOnly;
  display_device::configure_display(verify_config);
  EXPECT_EQ(manager_ptr->apply_call_count(), 1);
  EXPECT_FALSE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 1);

  // Reverting clears the active configuration so disconnect does not race with reapply.
  display_device::SingleDisplayConfiguration active_config;
  active_config.m_device_id = "{vdd}";
  active_config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  display_device::configure_display(active_config);
  EXPECT_EQ(manager_ptr->apply_call_count(), 2);

  display_device::revert_configuration();
  EXPECT_FALSE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 2);
}

TEST(DisplayDeviceReapply, ReappliesActiveConfigurationWithCooldownAndHandlesFailure) {
  [[maybe_unused]] const display_device_revert_guard_t guard;
  display_device::test_set_reapply_cooldown(50ms);
  const auto persistence {display_device::test_make_watched_persistence()};
  auto manager {std::make_unique<redock_settings_manager_t>(persistence)};
  auto *manager_ptr {manager.get()};
  display_device::test_install_settings_manager(std::move(manager), persistence);

  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "{vdd}";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  config.m_resolution = display_device::Resolution {3024, 1964};
  config.m_refresh_rate = display_device::Rational {120, 1};
  display_device::configure_display(config);
  EXPECT_EQ(manager_ptr->apply_call_count(), 1);

  EXPECT_TRUE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 2);
  ASSERT_TRUE(manager_ptr->latest_applied_config().has_value());
  EXPECT_EQ(manager_ptr->latest_applied_config()->m_device_id, "{vdd}");

  // Immediate repeat inside the cooldown window is skipped.
  EXPECT_FALSE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 2);

  // After cooldown expires, another reapply runs and propagates apply failure.
  std::this_thread::sleep_for(70ms);
  manager_ptr->set_apply_result(display_device::SettingsManagerInterface::ApplyResult::DevicePrepFailed);
  EXPECT_FALSE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 3);

  // Resetting persistence also clears active configuration.
  EXPECT_TRUE(display_device::reset_persistence());
  std::this_thread::sleep_for(70ms);
  manager_ptr->set_apply_result(display_device::SettingsManagerInterface::ApplyResult::Ok);
  EXPECT_FALSE(display_device::reapply_configuration_on_display_loss());
  EXPECT_EQ(manager_ptr->apply_call_count(), 3);
}
