/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// lib includes
#include <display_device/types.h>

// ffmpeg includes
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// local includes
#include <src/config.h>
#include <src/video.h>

using namespace std::literals;

/**
 * @brief Packet replacement rules retain the original non-owning byte views.
 */
TEST(VideoTouchPortTest, ScalesRotatedOutputsByTheirRotatedLogicalSize) {
  // 3000x1920 native pixels at scale 2: 1500x960 logical, or 960x1500 when rotated by 90 or 270 degrees.
  EXPECT_FLOAT_EQ(video::touch_port_scale(3000.0F, 1920.0F, 1500.0F, 960.0F, 0), 2.0F);
  EXPECT_FLOAT_EQ(video::touch_port_scale(3000.0F, 1920.0F, 960.0F, 1500.0F, 1), 2.0F);
  EXPECT_FLOAT_EQ(video::touch_port_scale(3000.0F, 1920.0F, 1500.0F, 960.0F, 2), 2.0F);
  EXPECT_FLOAT_EQ(video::touch_port_scale(3000.0F, 1920.0F, 960.0F, 1500.0F, 3), 2.0F);
}

TEST(VideoPacketReplacementTest, CopiesViewsWithoutCopyingTheirBackingBytes) {
  std::string original = "__old__";
  std::string replacement = "__new__";
  const video::packet_raw_t::replace_t rule {std::string_view(original).substr(2, 3), std::string_view(replacement).substr(2, 3)};
  EXPECT_EQ(rule.old, "old");
  EXPECT_EQ(rule._new, "new");
  EXPECT_EQ(rule.old.data(), original.data() + 2);
  EXPECT_EQ(rule._new.data(), replacement.data() + 2);
  original[2] = 'O';
  replacement[2] = 'N';
  EXPECT_EQ(rule.old, "Old");
  EXPECT_EQ(rule._new, "New");
}

TEST(VideoEncoderProbingTest, AllowsProbingWhenNoDevicesAreEnumerated) {
  EXPECT_TRUE(video::allow_encoder_probing({}));
}

TEST(VideoEncoderProbingTest, AllowsProbingWithAnActiveDevice) {
  std::vector<display_device::EnumeratedDevice> devices(2);
  devices[1].m_info = display_device::EnumeratedDevice::Info {};
  EXPECT_TRUE(video::allow_encoder_probing(devices));
}

TEST(VideoEncoderProbingTest, InactiveDevicesBlockProbingOnlyOnWindows) {
  // A sleeping display enumerates without additional info, i.e. as inactive.
  const std::vector<display_device::EnumeratedDevice> devices(2);
#ifdef _WIN32
  EXPECT_FALSE(video::allow_encoder_probing(devices));
#else
  EXPECT_TRUE(video::allow_encoder_probing(devices));
#endif
}

TEST(VideoFormatMappingTest, MapsSupportedHardwareDevicesAndRejectsUnsupportedOnes) {
  using enum platf::mem_type_e;
  const std::array cases {
    std::pair {AV_HWDEVICE_TYPE_D3D11VA, dxgi},
    std::pair {AV_HWDEVICE_TYPE_VAAPI, vaapi},
    std::pair {AV_HWDEVICE_TYPE_CUDA, cuda},
    std::pair {AV_HWDEVICE_TYPE_NONE, system},
#ifdef SUNSHINE_BUILD_VULKAN
    std::pair {AV_HWDEVICE_TYPE_VULKAN, vulkan},
#else
    std::pair {AV_HWDEVICE_TYPE_VULKAN, unknown},
#endif
    std::pair {AV_HWDEVICE_TYPE_DXVA2, unknown},
    std::pair {AV_HWDEVICE_TYPE_VIDEOTOOLBOX, unknown},
  };

  for (const auto &[format, expected] : cases) {
    EXPECT_EQ(video::map_base_dev_type(format), expected) << format;
  }
}

TEST(VideoFormatMappingTest, MapsSupportedPixelFormatsAndRejectsUnsupportedOnes) {
  using enum platf::pix_fmt_e;
  const std::array cases {
    std::pair {AV_PIX_FMT_VUYX, ayuv},
    std::pair {AV_PIX_FMT_XV30, y410},
    std::pair {AV_PIX_FMT_YUV420P10, yuv420p10},
    std::pair {AV_PIX_FMT_YUV420P, yuv420p},
    std::pair {AV_PIX_FMT_NV12, nv12},
    std::pair {AV_PIX_FMT_P010, p010},
    std::pair {AV_PIX_FMT_YUV444P, yuv444p},
    std::pair {AV_PIX_FMT_YUV444P16, yuv444p16},
    std::pair {AV_PIX_FMT_NONE, unknown},
    std::pair {AV_PIX_FMT_RGB24, unknown},
  };

  for (const auto &[format, expected] : cases) {
    EXPECT_EQ(video::map_pix_fmt(format), expected) << format;
  }
}

/**
 * @brief Each supported video format selects its matching encoder codec descriptor.
 */
TEST(VideoCodecSelectionTest, SelectsModernAndLegacyCodecDescriptors) {
  video::encoder_t encoder {};
  const std::array cases {
    std::pair {video::format_h264, &video::encoder_t::h264},
    std::pair {video::format_hevc, &video::encoder_t::hevc},
    std::pair {video::format_av1, &video::encoder_t::av1},
    std::pair {video::format_mpeg2, &video::encoder_t::mpeg2},
    std::pair {video::format_h263p, &video::encoder_t::h263p},
  };

  for (const auto &[format, codec] : cases) {
    video::config_t config {};
    config.videoFormat = format;
    EXPECT_EQ(&encoder.codec_from_config(config), &(encoder.*codec)) << format;
  }
}

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    BaseTest::SetUp();
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

/**
 * @brief Available encoder backends for this platform.
 */
const auto encoder_variants = std::to_array<video::encoder_t *>({
#if !defined(__APPLE__)
  &video::nvenc,
#endif
#ifdef _WIN32
  &video::amdvce,
  &video::quicksync,
#endif
#if defined(__linux__) || defined(__FreeBSD__)
  &video::vaapi,
#endif
#ifdef __APPLE__
  &video::videotoolbox,
#endif
  &video::software
});

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::ValuesIn(encoder_variants),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

#if !defined(_WIN32) && !defined(__APPLE__)
TEST(NvencAvcodecOptionsTest, UsesVersionStablePresetAndSpatialAqNames) {
  const std::array codecs {
    &video::nvenc.av1,
    &video::nvenc.hevc,
    &video::nvenc.h264,
  };

  for (const auto *codec : codecs) {
    const auto preset = std::ranges::find(codec->common_options, "preset"sv, &video::encoder_t::option_t::name);
    ASSERT_NE(codec->common_options.end(), preset);
    ASSERT_TRUE(std::holds_alternative<std::string *>(preset->value));
    EXPECT_EQ(&config::video.nv_legacy.preset, std::get<std::string *>(preset->value));

    const auto spatial_aq = std::ranges::find(codec->common_options, "spatial-aq"sv, &video::encoder_t::option_t::name);
    ASSERT_NE(codec->common_options.end(), spatial_aq);
    ASSERT_TRUE(std::holds_alternative<int *>(spatial_aq->value));
    EXPECT_EQ(&config::video.nv_legacy.spatial_aq, std::get<int *>(spatial_aq->value));

    EXPECT_EQ(
      codec->common_options.end(),
      std::ranges::find(codec->common_options, "aq"sv, &video::encoder_t::option_t::name)
    );
  }
}
#endif

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

/**
 * @brief Parameterized coverage for effective H.264 profile selection.
 */
struct H264ProfileTest: testing::TestWithParam<std::tuple<std::string_view, video::amf::coder_e, int, int>> {};

TEST_P(H264ProfileTest, SelectProfile) {
  const auto &[encoder_name, coder, chroma_sampling_type, expected_profile] = GetParam();
  video::config_t config {};
  config.chromaSamplingType = chroma_sampling_type;

  EXPECT_EQ(expected_profile, video::select_h264_profile(encoder_name, config, std::to_underlying(coder)));
}

INSTANTIATE_TEST_SUITE_P(
  H264ProfileTests,
  H264ProfileTest,
  testing::Values(
    std::make_tuple("h264_amf"sv, video::amf::coder_e::auto_, 0, AV_PROFILE_H264_HIGH),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cabac, 0, AV_PROFILE_H264_HIGH),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cavlc, 0, AV_PROFILE_H264_CONSTRAINED_BASELINE),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cavlc, 1, AV_PROFILE_H264_HIGH_444_PREDICTIVE),
    std::make_tuple("h264_nvenc"sv, video::amf::coder_e::cavlc, 0, AV_PROFILE_H264_HIGH)
  )
);

/**
 * @brief Parameterized coverage for resolving requested dynamic range against encoder capabilities.
 */
struct DynamicRangeTest: testing::TestWithParam<std::tuple<int, int, int, bool, bool, int>> {};

TEST_P(DynamicRangeTest, Resolve) {
  const auto &[video_format, chroma_sampling_type, requested_dynamic_range, supports_hdr, supports_hdr_yuv444, expected_dynamic_range] = GetParam();

  video::encoder_t encoder {
    "test"sv,
    {},
    {},
    {},
    {},
    {},
    {},
    0,
  };
  encoder.h264.name = "h264_test";
  encoder.hevc.name = "hevc_test";
  encoder.av1.name = "av1_test";

  video::config_t config {};
  config.videoFormat = video_format;
  config.dynamicRange = requested_dynamic_range;
  config.chromaSamplingType = chroma_sampling_type;

  auto *codec = &encoder.h264;
  if (video_format == 1) {
    codec = &encoder.hevc;
  } else if (video_format == 2) {
    codec = &encoder.av1;
  }
  (*codec)[video::encoder_t::DYNAMIC_RANGE] = supports_hdr;
  (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = supports_hdr_yuv444;

  const auto effective_config = video::resolve_dynamic_range(encoder, config);

  EXPECT_EQ(expected_dynamic_range, effective_config.dynamicRange);
  EXPECT_EQ(requested_dynamic_range, config.dynamicRange);
  EXPECT_EQ(video_format, effective_config.videoFormat);
  EXPECT_EQ(chroma_sampling_type, effective_config.chromaSamplingType);
}

INSTANTIATE_TEST_SUITE_P(
  DynamicRangeTests,
  DynamicRangeTest,
  testing::Values(
    std::make_tuple(0, 0, 1, false, true, 0),
    std::make_tuple(1, 0, 1, false, true, 0),
    std::make_tuple(1, 0, 1, true, false, 1),
    std::make_tuple(2, 1, 1, true, false, 0),
    std::make_tuple(2, 1, 1, false, true, 1),
    std::make_tuple(1, 0, 0, false, false, 0)
  )
);

/**
 * @brief Reset the published probe results before and after each test.
 */
struct EncoderProbeResultsTest: testing::Test {
  /**
   * @brief Start from an empty published state.
   */
  void SetUp() override {
    video::reset_encoder_probe_results();
  }

  /**
   * @brief Leave no published state behind for other tests.
   */
  void TearDown() override {
    video::reset_encoder_probe_results();
  }
};

TEST_F(EncoderProbeResultsTest, FailedCodecProbeDoesNotPublishCapabilities) {
  video::encoder_t encoder {"test"sv, {}, {}, {}, {}, {}, {}, 0};
  for (auto *codec : {&encoder.h264, &encoder.hevc, &encoder.av1}) {
    (*codec)[video::encoder_t::PASSED] = false;
    (*codec)[video::encoder_t::YUV444] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = true;
  }

  video::record_encoder_probe_results(encoder);

  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(video::last_encoder_probe_supported_yuv444_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[i]) << i;
  }
}

TEST_F(EncoderProbeResultsTest, SubsequentProbeReplacesPriorResults) {
  constexpr uint32_t ref_frames_invalidation = 1 << 8;  // Mirrors REF_FRAMES_INVALIDATION in video.cpp.
  video::encoder_t first {"first"sv, {}, {}, {}, {}, {}, {}, ref_frames_invalidation};
  for (auto *codec : {&first.h264, &first.hevc, &first.av1}) {
    (*codec)[video::encoder_t::PASSED] = true;
    (*codec)[video::encoder_t::YUV444] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = true;
  }

  // The second encoder only supports plain 8-bit 4:2:0 H.264.
  video::encoder_t second {"second"sv, {}, {}, {}, {}, {}, {}, 0};
  second.h264[video::encoder_t::PASSED] = true;

  video::record_encoder_probe_results(first);
  ASSERT_TRUE(video::last_encoder_probe_supported_ref_frames_invalidation);
  ASSERT_TRUE(video::last_encoder_probe_supported_yuv444_for_codec[1]);
  ASSERT_TRUE(video::last_encoder_probe_supported_dynamic_range_for_codec[2]);
  ASSERT_TRUE(video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[2]);

  video::record_encoder_probe_results(second);

  EXPECT_FALSE(video::last_encoder_probe_supported_ref_frames_invalidation);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(video::last_encoder_probe_supported_yuv444_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[i]) << i;
  }
}

#ifdef _WIN32
TEST(AmfH264OptionsTest, CoderUsesConfiguredValue) {
  const auto coder_option = std::ranges::find(video::amdvce.h264.common_options, "coder"sv, &video::encoder_t::option_t::name);

  ASSERT_NE(video::amdvce.h264.common_options.end(), coder_option);
  ASSERT_TRUE(std::holds_alternative<int *>(coder_option->value));
  EXPECT_EQ(&config::video.amd.amd_coder, std::get<int *>(coder_option->value));
}

/**
 * @brief Parameterized coverage for the AMF maximum access-unit-size option mappings.
 */
struct AmfMaxAuSizeOptionsTest: testing::TestWithParam<std::tuple<const video::encoder_t::codec_t *, bool>> {};

TEST_P(AmfMaxAuSizeOptionsTest, UsesConfiguredValueForSupportedCodecsOnly) {
  const auto &[codec, supported] = GetParam();
  const auto option = std::ranges::find(codec->common_options, "max_au_size"sv, &video::encoder_t::option_t::name);

  if (!supported) {
    EXPECT_EQ(codec->common_options.end(), option);
    return;
  }

  ASSERT_NE(codec->common_options.end(), option);
  ASSERT_TRUE(std::holds_alternative<std::optional<int> *>(option->value));
  EXPECT_EQ(&config::video.amd.amd_max_au_size, std::get<std::optional<int> *>(option->value));
}

INSTANTIATE_TEST_SUITE_P(
  AmfCodecOptions,
  AmfMaxAuSizeOptionsTest,
  testing::Values(
    std::make_tuple(&video::amdvce.h264, true),
    std::make_tuple(&video::amdvce.hevc, true),
    std::make_tuple(&video::amdvce.av1, false)
  )
);
#endif

using AmfMaxAuSizeConfigParam = std::tuple<std::string_view, std::optional<int>>;

/**
 * @brief Parameterized coverage for parsing and validating the AMF maximum access-unit size.
 */
struct AmfMaxAuSizeConfigTest: BaseTest, testing::WithParamInterface<AmfMaxAuSizeConfigParam> {
  void SetUp() override {
    BaseTest::SetUp();
    config::video.amd.amd_max_au_size.reset();
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/test_video.cpp";
  }

  void TearDown() override {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified_config_settings;
    BaseTest::TearDown();
  }

  config::video_t original_video {config::video};  ///< Video configuration restored after each parameterized test.
  config::audio_t original_audio {config::audio};  ///< Audio configuration restored after each parameterized test.
  config::stream_t original_stream {config::stream};  ///< Stream configuration restored after each parameterized test.
  config::nvhttp_t original_nvhttp {config::nvhttp};  ///< HTTP configuration restored after each parameterized test.
  config::input_t original_input {config::input};  ///< Input configuration restored after each parameterized test.
  config::sunshine_t original_sunshine {config::sunshine};  ///< Core configuration restored after each parameterized test.
  decltype(config::modified_config_settings) original_modified_config_settings {config::modified_config_settings};  ///< Modified settings restored after each parameterized test.
};

TEST_P(AmfMaxAuSizeConfigTest, AcceptsOnlyFfmpegSupportedRange) {
  const auto &[setting, expected] = GetParam();
  config::apply_config_for_test(setting);

  EXPECT_EQ(expected, config::video.amd.amd_max_au_size);
}

INSTANTIATE_TEST_SUITE_P(
  AmfMaxAuSizeValues,
  AmfMaxAuSizeConfigTest,
  testing::Values(
    AmfMaxAuSizeConfigParam {""sv, std::nullopt},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = -2\n"sv, std::nullopt},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = -1\n"sv, -1},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 0\n"sv, 0},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 800000\n"sv, 800000},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 2147483647\n"sv, std::numeric_limits<int>::max()}
  )
);

/**
 * @brief The software backend probes MPEG-2 even when its H.264 encoder is unavailable.
 */
TEST_F(PlatformTestSuite, SoftwareEncoderSupportsForcedMpeg2) {
  const auto h264_name = video::software.h264.name;
  auto restore = util::fail_guard([&] {
    video::software.h264.name = h264_name;
  });
  video::software.h264.name = "unavailable_h264_encoder";

  ASSERT_TRUE(video::validate_legacy_codec(video::software, video::format_mpeg2));
  EXPECT_TRUE(video::software.mpeg2[video::encoder_t::PASSED]);
}

/**
 * @brief The software backend probes H.263+ even when its H.264 encoder is unavailable.
 */
TEST_F(PlatformTestSuite, SoftwareEncoderSupportsForcedH263Plus) {
  const auto h264_name = video::software.h264.name;
  auto restore = util::fail_guard([&] {
    video::software.h264.name = h264_name;
  });
  video::software.h264.name = "unavailable_h264_encoder";

  ASSERT_TRUE(video::validate_legacy_codec(video::software, video::format_h263p));
  EXPECT_TRUE(video::software.h263p[video::encoder_t::PASSED]);
}

struct FramerateX100Test: BaseTest, testing::WithParamInterface<std::tuple<std::int32_t, AVRational>> {};

TEST_P(FramerateX100Test, Run) {
  const auto &[x100, expected] = GetParam();
  auto res = video::framerateX100_to_rational(x100);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, AVRational {24000, 1001}),
    std::make_tuple(2398, AVRational {24000, 1001}),
    std::make_tuple(2500, AVRational {25, 1}),
    std::make_tuple(2997, AVRational {30000, 1001}),
    std::make_tuple(3000, AVRational {30, 1}),
    std::make_tuple(5994, AVRational {60000, 1001}),
    std::make_tuple(6000, AVRational {60, 1}),
    std::make_tuple(11988, AVRational {120000, 1001}),
    std::make_tuple(23976, AVRational {240000, 1001}),  // future NTSC 240hz?
    std::make_tuple(9498, AVRational {4749, 50})  // from my LG 27GN950
  )
);

struct FramerateToRationalTest: testing::TestWithParam<std::tuple<int, int, AVRational>> {};

TEST_P(FramerateToRationalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  auto res = video::framerate_to_rational(config);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateToRationalTests,
  FramerateToRationalTest,
  testing::Values(
    std::make_tuple(60, 0, AVRational {60, 1}),  // no X100 value, fall back to integer framerate
    std::make_tuple(60, 5994, AVRational {60000, 1001}),
    std::make_tuple(120, 11988, AVRational {120000, 1001}),
    std::make_tuple(24, 2398, AVRational {24000, 1001})
  )
);

struct CaptureFrameIntervalTest: testing::TestWithParam<std::tuple<int, int, std::chrono::nanoseconds>> {};

TEST_P(CaptureFrameIntervalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  ASSERT_EQ(expected, video::capture_frame_interval(config));
}

INSTANTIATE_TEST_SUITE_P(
  CaptureFrameIntervalTests,
  CaptureFrameIntervalTest,
  testing::Values(
    std::make_tuple(60, 0, std::chrono::nanoseconds {16666666}),
    std::make_tuple(60, 5994, std::chrono::nanoseconds {16683333}),  // 1e9 * 1001 / 60000
    std::make_tuple(120, 11988, std::chrono::nanoseconds {8341666})  // 1e9 * 1001 / 120000
  )
);

/**
 * @brief Software encoder converts BGR0 and NV12 frames, including padded strides and
 *        backends that don't report the pixel pitch.
 */
TEST(SoftwareEncoderConversion, Bgr0AndNv12) {
  constexpr int w = 320;
  constexpr int h = 240;

  AVFrame *frame = av_frame_alloc();
  ASSERT_NE(frame, nullptr);
  frame->width = w;
  frame->height = h;
  frame->format = AV_PIX_FMT_YUV420P;

  video::avcodec_software_encode_device_t device;
  ASSERT_EQ(device.init(w, h, frame, AV_PIX_FMT_YUV420P, false), 0);
  // set_frame() takes ownership of the frame; the device frees it on destruction.
  ASSERT_EQ(device.set_frame(frame, nullptr), 0);

  // BGR0 frame (4 bytes per pixel) -- the classic KMS/DMABUF capture layout.
  std::vector<uint8_t> bgr0_buffer(static_cast<size_t>(w) * h * 4);
  platf::img_t bgr0_img {};
  bgr0_img.data = bgr0_buffer.data();
  bgr0_img.width = w;
  bgr0_img.height = h;
  bgr0_img.row_pitch = w * 4;
  bgr0_img.pixel_pitch = 4;
  EXPECT_EQ(device.convert(bgr0_img), 0);

  // NV12 frame (1 byte per pixel row pitch, Y plane + interleaved UV) -- the
  // layout delivered by PipeWire-based captures (KWin screencast / portal).
  std::vector<uint8_t> nv12_buffer(static_cast<size_t>(w) * h + static_cast<size_t>(w) * h / 2);
  platf::img_t nv12_img {};
  nv12_img.data = nv12_buffer.data();
  nv12_img.width = w;
  nv12_img.height = h;
  nv12_img.row_pitch = w;
  nv12_img.pixel_pitch = 1;
  EXPECT_EQ(device.convert(nv12_img), 0);

  // Padded-stride NV12 (alignment padding) -- the case the old row_pitch ==
  // width heuristic misdetected as BGR0, causing out-of-bounds reads.
  constexpr int padded_stride = w + 32;
  std::vector<uint8_t> padded_nv12_buffer(static_cast<size_t>(padded_stride) * h + static_cast<size_t>(padded_stride) * h / 2);
  platf::img_t padded_nv12_img {};
  padded_nv12_img.data = padded_nv12_buffer.data();
  padded_nv12_img.width = w;
  padded_nv12_img.height = h;
  padded_nv12_img.row_pitch = padded_stride;
  padded_nv12_img.pixel_pitch = 1;
  EXPECT_EQ(device.convert(padded_nv12_img), 0);

  // Capture backends that don't report pixel_pitch fall back to deriving it
  // from the row pitch (1 byte per pixel = NV12, 4 = BGR0).
  platf::img_t fallback_bgr0_img {};
  fallback_bgr0_img.data = bgr0_buffer.data();
  fallback_bgr0_img.width = w;
  fallback_bgr0_img.height = h;
  fallback_bgr0_img.row_pitch = w * 4;
  EXPECT_EQ(device.convert(fallback_bgr0_img), 0);

  platf::img_t fallback_nv12_img {};
  fallback_nv12_img.data = nv12_buffer.data();
  fallback_nv12_img.width = w;
  fallback_nv12_img.height = h;
  fallback_nv12_img.row_pitch = w;
  EXPECT_EQ(device.convert(fallback_nv12_img), 0);
}

/**
 * @brief Fixture for the frame timestamp bookkeeping helpers.
 */
struct FrameTimestampQueueTest: testing::Test {
  /**
   * @brief Produce a distinct timestamp for a frame number.
   *
   * @param frame_nr Frame number to derive the timestamp from.
   * @return Timestamp unique to the frame number.
   */
  static std::chrono::steady_clock::time_point ts(int64_t frame_nr) {
    return std::chrono::steady_clock::time_point {std::chrono::milliseconds {frame_nr}};
  }

  video::frame_timestamp_queue_t queue;
};

TEST_F(FrameTimestampQueueTest, MatchesInOrder) {
  for (int64_t frame = 1; frame <= 4; ++frame) {
    video::remember_frame_timestamp(queue, frame, ts(frame));
    EXPECT_EQ(ts(frame), video::match_frame_timestamp(queue, frame));
  }
  EXPECT_TRUE(queue.empty());
}

TEST_F(FrameTimestampQueueTest, MatchesDelayedPackets) {
  // Encoders may deliver packets several frames behind submission.
  video::remember_frame_timestamp(queue, 1, ts(1));
  video::remember_frame_timestamp(queue, 2, ts(2));
  video::remember_frame_timestamp(queue, 3, ts(3));

  EXPECT_EQ(ts(1), video::match_frame_timestamp(queue, 1));

  video::remember_frame_timestamp(queue, 4, ts(4));

  EXPECT_EQ(ts(2), video::match_frame_timestamp(queue, 2));
  EXPECT_EQ(ts(3), video::match_frame_timestamp(queue, 3));
  EXPECT_EQ(ts(4), video::match_frame_timestamp(queue, 4));
}

TEST_F(FrameTimestampQueueTest, DiscardsDroppedPts) {
  // Frames whose packets never surface must be discarded, and a stale pts
  // must never be paired with another frame's timestamp.
  video::remember_frame_timestamp(queue, 1, ts(1));
  video::remember_frame_timestamp(queue, 2, ts(2));
  video::remember_frame_timestamp(queue, 3, ts(3));

  EXPECT_EQ(ts(3), video::match_frame_timestamp(queue, 3));
  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 1));
  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 2));
}

TEST_F(FrameTimestampQueueTest, IgnoresUnknownPts) {
  video::remember_frame_timestamp(queue, 5, ts(5));

  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 6));
  EXPECT_TRUE(queue.empty());
}

TEST_F(FrameTimestampQueueTest, SkipsMissingTimestamps) {
  // Duplicate or synthesized frames carry no capture timestamp; they must not
  // enqueue anything or disturb their neighbors.
  video::remember_frame_timestamp(queue, 1, std::nullopt);
  video::remember_frame_timestamp(queue, 2, ts(2));

  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 1));
  EXPECT_EQ(ts(2), video::match_frame_timestamp(queue, 2));
}

TEST_F(FrameTimestampQueueTest, EvictsOldestBeyondCap) {
  // A wedged encoder must not grow the queue without bound.
  for (int64_t frame = 1; frame <= 200; ++frame) {
    video::remember_frame_timestamp(queue, frame, ts(frame));
  }

  EXPECT_EQ(128u, queue.size());
  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 1));  // evicted
  EXPECT_EQ(ts(73), video::match_frame_timestamp(queue, 73));  // oldest survivor
  EXPECT_EQ(ts(200), video::match_frame_timestamp(queue, 200));
}

TEST_F(FrameTimestampQueueTest, EmptyQueueYieldsNothing) {
  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 1));
}

TEST_F(FrameTimestampQueueTest, ConsumesEntriesOnce) {
  video::remember_frame_timestamp(queue, 1, ts(1));

  EXPECT_EQ(ts(1), video::match_frame_timestamp(queue, 1));
  EXPECT_EQ(std::nullopt, video::match_frame_timestamp(queue, 1));
}
