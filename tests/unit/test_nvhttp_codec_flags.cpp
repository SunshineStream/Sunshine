/**
 * @file tests/unit/test_nvhttp_codec_flags.cpp
 * @brief Test the codec capability bitmask advertised to clients.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <utility>

// local includes
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/video.h>

namespace {
  /**
   * @brief Codec under test, indexed like the `video::last_encoder_probe_*` arrays.
   */
  enum class codec_e {
    hevc = 1,  ///< HEVC.
    av1 = 2,  ///< AV1.
  };

  /**
   * @brief Probe results and configured mode for one codec.
   */
  struct flags_case_t {
    codec_e codec;  ///< Codec under test.
    int mode;  ///< Configured `hevc_mode` or `av1_mode`.
    bool yuv444;  ///< 8-bit 4:4:4 probe result.
    bool dynamic_range;  ///< 10-bit 4:2:0 probe result.
    bool dynamic_range_yuv444;  ///< 10-bit 4:4:4 probe result.
    std::uint32_t expected;  ///< Expected flags within the codec's mask.
  };

  constexpr std::uint32_t HEVC_MASK = SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444;
  constexpr std::uint32_t AV1_MASK = SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444;
}  // namespace

/**
 * @brief Save and restore the global probe state so no encoder hardware is needed.
 */
class CodecProbeStateTest: public testing::Test {
protected:
  /**
   * @brief Save the global probe state.
   */
  void SetUp() override {
    hevc_mode_ = video::active_hevc_mode;
    av1_mode_ = video::active_av1_mode;
    yuv444_ = video::last_encoder_probe_supported_yuv444_for_codec;
    dynamic_range_ = video::last_encoder_probe_supported_dynamic_range_for_codec;
    dynamic_range_yuv444_ = video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec;
  }

  /**
   * @brief Restore the global probe state.
   */
  void TearDown() override {
    video::active_hevc_mode = hevc_mode_;
    video::active_av1_mode = av1_mode_;
    video::last_encoder_probe_supported_yuv444_for_codec = yuv444_;
    video::last_encoder_probe_supported_dynamic_range_for_codec = dynamic_range_;
    video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec = dynamic_range_yuv444_;
  }

private:
  int hevc_mode_ = 0;  ///< Saved HEVC mode.
  int av1_mode_ = 0;  ///< Saved AV1 mode.
  std::array<bool, 3> yuv444_ {};  ///< Saved 8-bit 4:4:4 results.
  std::array<bool, 3> dynamic_range_ {};  ///< Saved 10-bit 4:2:0 results.
  std::array<bool, 3> dynamic_range_yuv444_ {};  ///< Saved 10-bit 4:4:4 results.
};

/**
 * @brief Parameterized probe results and modes applied directly to the global state.
 */
class CodecModeFlagsTest: public CodecProbeStateTest, public testing::WithParamInterface<flags_case_t> {};

/**
 * @brief End-to-end probe-to-bitmask coverage using a hand-built encoder.
 */
using CodecProbeAdvertisementTest = CodecProbeStateTest;

TEST_F(CodecProbeAdvertisementTest, Probed8Bit444Without10Bit444AdvertisesOnly8Bit444) {
  using namespace std::literals;

  video::encoder_t encoder {"test"sv, {}, {}, {}, {}, {}, {}, 0};
  for (auto *codec : {&encoder.h264, &encoder.hevc, &encoder.av1}) {
    (*codec)[video::encoder_t::PASSED] = true;
    (*codec)[video::encoder_t::YUV444] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = false;
  }

  video::active_hevc_mode = 5;
  video::active_av1_mode = 5;
  video::reset_encoder_probe_results();
  video::record_encoder_probe_results(encoder);

  constexpr std::uint32_t expected = SCM_H264 | SCM_H264_HIGH8_444 |
                                     SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444 |
                                     SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_HIGH8_444;
  const auto flags = nvhttp::get_codec_mode_flags();
  EXPECT_EQ(expected, flags);
  EXPECT_EQ(0u, flags & (SCM_HEVC_REXT10_444 | SCM_AV1_HIGH10_444));
}

TEST_P(CodecModeFlagsTest, AdvertisesOnlyProbedAndConfiguredModes) {
  using enum codec_e;
  const auto &c = GetParam();
  const auto index = static_cast<std::size_t>(std::to_underlying(c.codec));

  video::active_hevc_mode = c.codec == hevc ? c.mode : 1;
  video::active_av1_mode = c.codec == av1 ? c.mode : 1;
  video::last_encoder_probe_supported_yuv444_for_codec = {};
  video::last_encoder_probe_supported_dynamic_range_for_codec = {};
  video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec = {};
  video::last_encoder_probe_supported_yuv444_for_codec[index] = c.yuv444;
  video::last_encoder_probe_supported_dynamic_range_for_codec[index] = c.dynamic_range;
  video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[index] = c.dynamic_range_yuv444;

  const auto mask = c.codec == hevc ? HEVC_MASK : AV1_MASK;
  EXPECT_EQ(c.expected, nvhttp::get_codec_mode_flags() & mask);
}

INSTANTIATE_TEST_SUITE_P(
  CodecModeFlagsTests,
  CodecModeFlagsTest,
  testing::Values(
    // 8-bit 4:4:4 passes but 10-bit 4:4:4 fails.
    flags_case_t {codec_e::hevc, 5, true, true, false, SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444},
    flags_case_t {codec_e::av1, 5, true, true, false, SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_HIGH8_444},
    // 10-bit 4:4:4 passes but 10-bit 4:2:0 fails under mode 5.
    flags_case_t {codec_e::hevc, 5, true, false, true, SCM_HEVC | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444},
    flags_case_t {codec_e::av1, 5, true, false, true, SCM_AV1_MAIN8 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444},
    // 10-bit 4:4:4 passes but 8-bit 4:4:4 fails.
    flags_case_t {codec_e::hevc, 4, false, true, true, SCM_HEVC | SCM_HEVC_REXT10_444},
    flags_case_t {codec_e::av1, 4, false, true, true, SCM_AV1_MAIN8 | SCM_AV1_HIGH10_444},
    // Mode 3 never advertises 10-bit 4:4:4, whatever the probe found.
    flags_case_t {codec_e::hevc, 3, true, true, true, SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444},
    flags_case_t {codec_e::av1, 3, true, true, true, SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_HIGH8_444}
  )
);
