/**
 * @file tests/unit/platform/linux/test_cuda_color.cpp
 * @brief Tests for the 8-bit luma and chroma samples written by the CUDA NV12 conversion.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <array>
  #include <cmath>
  #include <cstdint>
  #include <cstdio>
  #include <limits>
  #include <string>
  #include <vector>

  // local includes
  #include "src/platform/linux/cuda_color.h"
  #include "src/video_colorspace.h"

  #ifdef SUNSHINE_BUILD_CUDA
    #include "src/platform/linux/cuda.h"
  #endif

namespace {
  /**
   * @brief An 8-bit RGB color.
   */
  struct rgb8_t {
    std::uint8_t r;  ///< Red.
    std::uint8_t g;  ///< Green.
    std::uint8_t b;  ///< Blue.
  };

  constexpr rgb8_t pure_red {0xff, 0x00, 0x00};  ///< Turned green at full range before the clamp.
  constexpr rgb8_t pure_blue {0x00, 0x00, 0xff};  ///< Turned dark green at full range before the clamp.

  /**
   * @brief Primaries, secondaries, black, white, gray, and the 254 variants of red and blue.
   */
  constexpr std::array test_colors {
    pure_red,
    rgb8_t {0xfe, 0x00, 0x00},
    rgb8_t {0x00, 0xff, 0x00},
    pure_blue,
    rgb8_t {0x00, 0x00, 0xfe},
    rgb8_t {0xff, 0xff, 0x00},
    rgb8_t {0x00, 0xff, 0xff},
    rgb8_t {0xff, 0x00, 0xff},
    rgb8_t {0x00, 0x00, 0x00},
    rgb8_t {0xff, 0xff, 0xff},
    rgb8_t {0x80, 0x80, 0x80},
  };

  /**
   * @brief Every 8-bit colorspace the NV12 kernel can be given.
   */
  constexpr std::array nv12_colorspaces {
    video::sunshine_colorspace_t {video::colorspace_e::rec601, false, 8},
    video::sunshine_colorspace_t {video::colorspace_e::rec601, true, 8},
    video::sunshine_colorspace_t {video::colorspace_e::rec709, false, 8},
    video::sunshine_colorspace_t {video::colorspace_e::rec709, true, 8},
    video::sunshine_colorspace_t {video::colorspace_e::bt2020sdr, false, 8},
    video::sunshine_colorspace_t {video::colorspace_e::bt2020sdr, true, 8},
  };

  /**
   * @brief U and V of one pixel, computed the way `calcUV` in cuda.cu computes them.
   *
   * @param colorspace The colorspace whose unorm color matrix the kernel uses.
   * @param color The pixel.
   * @return Normalized U and V.
   */
  std::array<float, 2> unorm_chroma(const video::sunshine_colorspace_t &colorspace, rgb8_t color) {
    const auto &m = *video::color_vectors_from_colorspace(colorspace, true);
    const float r = color.r / 255.0f;
    const float g = color.g / 255.0f;
    const float b = color.b / 255.0f;
    const float u = r * m.color_vec_u[0] + g * m.color_vec_u[1] + b * m.color_vec_u[2] + m.color_vec_u[3];
    const float v = r * m.color_vec_v[0] + g * m.color_vec_v[1] + b * m.color_vec_v[2] + m.color_vec_v[3];
    return {u * m.range_uv[0] + m.range_uv[1], v * m.range_uv[0] + m.range_uv[1]};
  }

  /**
   * @brief The 8-bit U and V that ITU-T H.273 gives for a color, rounded and clamped.
   *
   * @param colorspace The colorspace.
   * @param color The color.
   * @return U and V in 8-bit code values.
   */
  std::array<int, 2> reference_chroma(const video::sunshine_colorspace_t &colorspace, rgb8_t color) {
    double kr = 0.2126;
    double kb = 0.0722;
    if (colorspace.colorspace == video::colorspace_e::rec601) {
      kr = 0.299;
      kb = 0.114;
    } else if (colorspace.colorspace == video::colorspace_e::bt2020sdr) {
      kr = 0.2627;
      kb = 0.0593;
    }
    const double r = color.r / 255.0;
    const double g = color.g / 255.0;
    const double b = color.b / 255.0;
    const double y = kr * r + (1.0 - kr - kb) * g + kb * b;
    const double scale = colorspace.full_range ? 255.0 : 224.0;
    const auto quantize = [scale](double chroma) {
      return std::clamp(static_cast<int>(std::lround(128.0 + scale * chroma)), 0, 255);
    };
    return {quantize((b - y) / (2.0 * (1.0 - kb))), quantize((r - y) / (2.0 * (1.0 - kr)))};
  }

  /**
   * @brief The NV12 chroma sample of a 2x2 block filled with one color.
   *
   * @param colorspace The colorspace.
   * @param color The color of all four pixels.
   * @return U and V as written to the chroma plane.
   */
  std::array<int, 2> solid_block_chroma(const video::sunshine_colorspace_t &colorspace, rgb8_t color) {
    const auto [u, v] = unorm_chroma(colorspace, color);
    return {cuda::nv12_chroma_sample(u, u, u, u), cuda::nv12_chroma_sample(v, v, v, v)};
  }

  /**
   * @brief Describe a test case for failure messages.
   *
   * @param colorspace The colorspace.
   * @param color The color.
   * @return A description such as "SDR (Rec. 709) full #ff0000".
   */
  std::string describe(const video::sunshine_colorspace_t &colorspace, rgb8_t color) {
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", color.r, color.g, color.b);
    return std::string(video::colorspace_to_string(colorspace.colorspace)) + (colorspace.full_range ? " full " : " limited ") + hex;
  }
}  // namespace

/**
 * @brief Samples outside [0, 255] are clamped instead of wrapping.
 */
TEST(CudaColorTest, ClampsSamplesToTheUint8Range) {
  const auto clamp = [](float value) {
    return static_cast<int>(cuda::clamp_to_uint8(value));
  };
  EXPECT_EQ(clamp(-1000.0f), 0);
  EXPECT_EQ(clamp(-0.5f), 0);
  EXPECT_EQ(clamp(0.0f), 0);
  EXPECT_EQ(clamp(0.99f), 0);
  EXPECT_EQ(clamp(1.0f), 1);
  EXPECT_EQ(clamp(127.9f), 127);
  EXPECT_EQ(clamp(254.99f), 254);
  EXPECT_EQ(clamp(255.0f), 255);
  EXPECT_EQ(clamp(255.99f), 255);
  EXPECT_EQ(clamp(256.0f), 255);
  EXPECT_EQ(clamp(256.5f), 255);
  EXPECT_EQ(clamp(1000.0f), 255);
  EXPECT_EQ(clamp(std::numeric_limits<float>::infinity()), 255);
  EXPECT_EQ(clamp(-std::numeric_limits<float>::infinity()), 0);
  EXPECT_EQ(clamp(std::numeric_limits<float>::quiet_NaN()), 0);
}

/**
 * @brief Normalized NV12 luma rounds to the nearest 8-bit code value without wrapping.
 */
TEST(CudaColorTest, Nv12LumaRoundsAndClamps) {
  EXPECT_EQ(cuda::nv12_luma_sample(-0.1f), 0);
  EXPECT_EQ(cuda::nv12_luma_sample(0.0f), 0);
  EXPECT_EQ(cuda::nv12_luma_sample(63.75f / 255.0f), 64);
  EXPECT_EQ(cuda::nv12_luma_sample(64.25f / 255.0f), 64);
  EXPECT_EQ(cuda::nv12_luma_sample(0.5f), 128);
  EXPECT_EQ(cuda::nv12_luma_sample(1.0f), 255);
  EXPECT_EQ(cuda::nv12_luma_sample(1.1f), 255);
}

/**
 * @brief Full-range pure red keeps the maximum V and pure blue the maximum U.
 *
 * Their scaled chroma is above 255, which wrapped to 0 and showed red as green and blue as dark green.
 */
TEST(CudaColorTest, FullRangePureRedAndBlueKeepMaximumChroma) {
  for (auto colorspace : nv12_colorspaces) {
    if (!colorspace.full_range) {
      continue;
    }
    SCOPED_TRACE(video::colorspace_to_string(colorspace.colorspace));

    const float red_v = unorm_chroma(colorspace, pure_red)[1];
    const float blue_u = unorm_chroma(colorspace, pure_blue)[0];
    ASSERT_GT(red_v * 256.0f, 255.0f) << "the case this test guards no longer overflows";
    ASSERT_GT(blue_u * 256.0f, 255.0f) << "the case this test guards no longer overflows";

    EXPECT_EQ(static_cast<int>(cuda::nv12_chroma_sample(red_v, red_v, red_v, red_v)), 255);
    EXPECT_EQ(static_cast<int>(cuda::nv12_chroma_sample(blue_u, blue_u, blue_u, blue_u)), 255);
  }
}

/**
 * @brief Each solid color gets the chroma its colorspace defines, within one code value.
 */
TEST(CudaColorTest, Nv12ChromaMatchesTheColorspaceMatrix) {
  for (auto colorspace : nv12_colorspaces) {
    for (auto color : test_colors) {
      SCOPED_TRACE(describe(colorspace, color));
      const auto expected = reference_chroma(colorspace, color);
      const auto actual = solid_block_chroma(colorspace, color);
      EXPECT_NEAR(actual[0], expected[0], 1) << "U";
      EXPECT_NEAR(actual[1], expected[1], 1) << "V";
    }
  }
}

/**
 * @brief Limited-range chroma stays within the 16 to 240 range.
 */
TEST(CudaColorTest, LimitedRangeChromaStaysInRange) {
  for (auto colorspace : nv12_colorspaces) {
    if (colorspace.full_range) {
      continue;
    }
    for (auto color : test_colors) {
      SCOPED_TRACE(describe(colorspace, color));
      for (int sample : solid_block_chroma(colorspace, color)) {
        EXPECT_GE(sample, 16);
        EXPECT_LE(sample, 240);
      }
    }
  }
}

/**
 * @brief A block's chroma is the average of its four pixels, wherever each pixel is.
 */
TEST(CudaColorTest, Nv12ChromaAveragesTheFourPixels) {
  const video::sunshine_colorspace_t colorspace {video::colorspace_e::rec709, true, 8};
  const float red = unorm_chroma(colorspace, pure_red)[1];
  const float black = unorm_chroma(colorspace, {0x00, 0x00, 0x00})[1];

  // Chroma is linear in RGB, so half red and half black averages to the chroma of #800000 (127.5).
  const int expected = reference_chroma(colorspace, {0x80, 0x00, 0x00})[1];
  const std::array<std::array<float, 4>, 3> layouts {{
    {red, red, black, black},
    {red, black, red, black},
    {red, black, black, red},
  }};
  for (const auto &[lt, rt, lb, rb] : layouts) {
    EXPECT_NEAR(cuda::nv12_chroma_sample(lt, rt, lb, rb), expected, 1);
  }

  // Three overflowing pixels and one black one do not overflow on average.
  EXPECT_LT(static_cast<int>(cuda::nv12_chroma_sample(red, red, red, black)), 255);
}

  #ifdef SUNSHINE_BUILD_CUDA
namespace {
  /**
   * @brief A frame filled with one color, in the B, G, R, A byte order of the CUDA texture.
   *
   * @param color The color.
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @return The frame pixels.
   */
  std::vector<std::uint8_t> solid_bgra_frame(rgb8_t color, int width, int height) {
    std::vector<std::uint8_t> frame;
    frame.reserve(static_cast<std::size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
      frame.insert(frame.end(), {color.b, color.g, color.r, 0xff});
    }
    return frame;
  }
}  // namespace

/**
 * @brief The NV12 kernel preserves neutral luma across both full and limited range.
 *
 * This runs the kernel on the GPU, so it is skipped on hosts without a CUDA device.
 */
TEST(CudaNv12KernelTest, PreservesNeutralLumaInBothRanges) {
  if (!cuda::test::has_device()) {
    GTEST_SKIP() << "No CUDA device";
  }

  constexpr int width = 16;
  constexpr int height = 8;
  constexpr std::array<std::uint8_t, 4> levels {0, 64, 128, 255};
  constexpr std::array<std::uint8_t, 4> limited_codes {16, 71, 126, 235};
  for (auto colorspace : nv12_colorspaces) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
      const rgb8_t gray {levels[i], levels[i], levels[i]};
      SCOPED_TRACE(describe(colorspace, gray));
      const auto planes = cuda::test::convert_to_nv12(solid_bgra_frame(gray, width, height), width, height, colorspace);
      ASSERT_TRUE(planes.has_value());
      ASSERT_EQ(planes->y.size(), static_cast<std::size_t>(width) * height);
      const auto expected = colorspace.full_range ? levels[i] : limited_codes[i];
      for (std::size_t pixel = 0; pixel < planes->y.size(); ++pixel) {
        ASSERT_EQ(planes->y[pixel], expected) << "Y at pixel " << pixel;
      }
    }
  }
}

/**
 * @brief The NV12 kernel writes the test card's colors with the chroma their colorspace defines.
 *
 * This runs the kernel on the GPU, so it is skipped on hosts without a CUDA device.
 */
TEST(CudaNv12KernelTest, WritesChromaThatMatchesTheColorspaceMatrix) {
  if (!cuda::test::has_device()) {
    GTEST_SKIP() << "No CUDA device";
  }

  constexpr int width = 16;
  constexpr int height = 8;
  for (auto colorspace : nv12_colorspaces) {
    for (auto color : test_colors) {
      SCOPED_TRACE(describe(colorspace, color));
      const auto planes = cuda::test::convert_to_nv12(solid_bgra_frame(color, width, height), width, height, colorspace);
      ASSERT_TRUE(planes.has_value());
      ASSERT_EQ(planes->uv.size(), static_cast<std::size_t>(width) * height / 2);

      const auto expected = reference_chroma(colorspace, color);
      EXPECT_NEAR(planes->uv[0], expected[0], 1) << "U";
      EXPECT_NEAR(planes->uv[1], expected[1], 1) << "V";

      // A solid frame has the same chroma in every block.
      for (std::size_t i = 2; i < planes->uv.size(); i += 2) {
        ASSERT_EQ(static_cast<int>(planes->uv[i]), static_cast<int>(planes->uv[0])) << "U at block " << i / 2;
        ASSERT_EQ(static_cast<int>(planes->uv[i + 1]), static_cast<int>(planes->uv[1])) << "V at block " << i / 2;
      }
    }
  }
}

/**
 * @brief Full-range pure red and pure blue reach the GPU's chroma plane at the maximum, not wrapped to 0.
 */
TEST(CudaNv12KernelTest, KeepsFullRangePureRedAndBlue) {
  if (!cuda::test::has_device()) {
    GTEST_SKIP() << "No CUDA device";
  }

  constexpr int width = 16;
  constexpr int height = 8;
  const video::sunshine_colorspace_t colorspace {video::colorspace_e::rec709, true, 8};

  const auto red = cuda::test::convert_to_nv12(solid_bgra_frame(pure_red, width, height), width, height, colorspace);
  ASSERT_TRUE(red.has_value());
  EXPECT_EQ(static_cast<int>(red->uv[1]), 255) << "V of #ff0000";

  const auto blue = cuda::test::convert_to_nv12(solid_bgra_frame(pure_blue, width, height), width, height, colorspace);
  ASSERT_TRUE(blue.has_value());
  EXPECT_EQ(static_cast<int>(blue->uv[0]), 255) << "U of #0000ff";
}
  #endif

#endif
