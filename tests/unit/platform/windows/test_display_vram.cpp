/**
 * @file tests/unit/platform/windows/test_display_vram.cpp
 * @brief Tests for Windows FP16 shader selection and cursor mask conversion.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <array>
  #include <bit>
  #include <cstdint>
  #include <tuple>

  // local includes
  #include <src/platform/windows/display.h>

namespace platf::dxgi {
  /**
   * @brief Convert cursor mask bytes to pixels for XOR blending.
   *
   * @param img_data Raw DXGI cursor data.
   * @param shape_info Cursor format and dimensions.
   * @return Cursor pixels for XOR blending.
   */
  util::buffer_t<std::uint8_t> make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info);

  /**
   * @brief Convert cursor mask bytes to pixels for alpha blending.
   *
   * @param img_data Raw DXGI cursor data.
   * @param shape_info Cursor format and dimensions.
   * @return Cursor pixels for alpha blending.
   */
  util::buffer_t<std::uint8_t> make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info);
}  // namespace platf::dxgi

TEST(WindowsCursorTest, DecodesEveryMonochromeMaskCombinationAcrossByteBoundaries) {
  // Each pair of AND/XOR bits selects black, white, transparent, or inverted.
  util::buffer_t<std::uint8_t> masks {4};
  masks[0] = 0x33;
  masks[1] = 0x33;
  masks[2] = 0x55;
  masks[3] = 0x55;
  const DXGI_OUTDUPL_POINTER_SHAPE_INFO shape {
    .Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME,
    .Width = 16,
    .Height = 2,
    .Pitch = 2,
  };
  const auto alpha = platf::dxgi::make_cursor_alpha_image(masks, shape);
  const auto xor_image = platf::dxgi::make_cursor_xor_image(masks, shape);
  ASSERT_EQ(alpha.size(), 16U * sizeof(std::uint32_t));
  ASSERT_EQ(xor_image.size(), alpha.size());
  const std::array<std::uint32_t, 4> expected_alpha {0xFF000000, 0xFFFFFFFF, 0, 0};
  const std::array<std::uint32_t, 4> expected_xor {0, 0, 0, 0xFFFFFFFF};
  for (std::size_t pixel = 0; pixel < 16; ++pixel) {
    std::array<std::uint8_t, 4> alpha_bytes;
    std::array<std::uint8_t, 4> xor_bytes;
    std::copy_n(alpha.begin() + pixel * 4, 4, alpha_bytes.begin());
    std::copy_n(xor_image.begin() + pixel * 4, 4, xor_bytes.begin());
    EXPECT_EQ(std::bit_cast<std::uint32_t>(alpha_bytes), expected_alpha[pixel % 4]);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(xor_bytes), expected_xor[pixel % 4]);
  }
}

TEST(WindowsCursorTest, StopsAtTheLastPixelInAPartialMaskByte) {
  util::buffer_t<std::uint8_t> masks {2};
  masks[0] = 0xFF;
  masks[1] = 0xFF;
  const DXGI_OUTDUPL_POINTER_SHAPE_INFO shape {
    .Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME,
    .Width = 3,
    .Height = 2,
    .Pitch = 1,
  };
  const auto alpha = platf::dxgi::make_cursor_alpha_image(masks, shape);
  const auto xor_image = platf::dxgi::make_cursor_xor_image(masks, shape);
  EXPECT_EQ(alpha.size(), 12U);
  EXPECT_EQ(xor_image.size(), 12U);
  EXPECT_TRUE(std::all_of(alpha.begin(), alpha.end(), [](auto byte) {
    return byte == 0;
  }));
  EXPECT_TRUE(std::all_of(xor_image.begin(), xor_image.end(), [](auto byte) {
    return byte == 0xFF;
  }));
}

TEST(WindowsCursorTest, ProcessesEveryMaskedColorPixelAndPreservesTheInput) {
  const std::array<std::uint32_t, 3> pixels {0xFF112233, 0x00123456, 0x7F445566};
  const auto bytes = std::bit_cast<std::array<std::uint8_t, 12>>(pixels);
  util::buffer_t<std::uint8_t> input {bytes.size()};
  std::ranges::copy(bytes, input.begin());
  const DXGI_OUTDUPL_POINTER_SHAPE_INFO shape {
    .Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR,
    .Width = 3,
    .Height = 1,
    .Pitch = 12,
  };

  const auto alpha = platf::dxgi::make_cursor_alpha_image(input, shape);
  const auto xor_image = platf::dxgi::make_cursor_xor_image(input, shape);
  const std::array<std::uint32_t, 3> expected_alpha {0, 0xFF123456, 0x7F445566};
  const std::array<std::uint32_t, 3> expected_xor {0xFF112233, 0, 0x7F445566};
  EXPECT_TRUE((std::ranges::equal(alpha, std::bit_cast<std::array<std::uint8_t, 12>>(expected_alpha))));
  EXPECT_TRUE((std::ranges::equal(xor_image, std::bit_cast<std::array<std::uint8_t, 12>>(expected_xor))));
  EXPECT_TRUE(std::ranges::equal(input, bytes));
}

using platf::dxgi::fp16_transfer_e;
using platf::dxgi::select_fp16_transfer;

/**
 * @brief Encoder output format, HDR state, option state, and expected FP16 transfer.
 */
using Fp16TransferParam = std::tuple<DXGI_FORMAT, bool, bool, fp16_transfer_e>;

/**
 * @brief Parameterized coverage for every output format with HDR and the opt-in option toggled.
 */
struct Fp16TransferTest: testing::TestWithParam<Fp16TransferParam> {};

TEST_P(Fp16TransferTest, SelectsExpectedTransfer) {
  const auto &[format, hdr, gamma_encoded_sdr, expected] = GetParam();

  EXPECT_EQ(expected, select_fp16_transfer(format, hdr, gamma_encoded_sdr));
}

INSTANTIATE_TEST_SUITE_P(
  LinearSdrByDefault,
  Fp16TransferTest,
  testing::Values(
    Fp16TransferParam {DXGI_FORMAT_NV12, false, false, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_P010, false, false, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_R16_UINT, false, false, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_AYUV, false, false, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_Y410, false, false, fp16_transfer_e::linear}
  )
);

INSTANTIATE_TEST_SUITE_P(
  GammaEncodedSdrWhenEnabled,
  Fp16TransferTest,
  testing::Values(
    Fp16TransferParam {DXGI_FORMAT_NV12, false, true, fp16_transfer_e::gamma_encoded},
    Fp16TransferParam {DXGI_FORMAT_P010, false, true, fp16_transfer_e::gamma_encoded},
    Fp16TransferParam {DXGI_FORMAT_R16_UINT, false, true, fp16_transfer_e::gamma_encoded},
    Fp16TransferParam {DXGI_FORMAT_AYUV, false, true, fp16_transfer_e::gamma_encoded},
    Fp16TransferParam {DXGI_FORMAT_Y410, false, true, fp16_transfer_e::gamma_encoded}
  )
);

INSTANTIATE_TEST_SUITE_P(
  HdrUnaffectedByOption,
  Fp16TransferTest,
  testing::Values(
    Fp16TransferParam {DXGI_FORMAT_NV12, true, false, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_NV12, true, true, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_P010, true, false, fp16_transfer_e::perceptual_quantizer},
    Fp16TransferParam {DXGI_FORMAT_P010, true, true, fp16_transfer_e::perceptual_quantizer},
    Fp16TransferParam {DXGI_FORMAT_R16_UINT, true, false, fp16_transfer_e::perceptual_quantizer},
    Fp16TransferParam {DXGI_FORMAT_R16_UINT, true, true, fp16_transfer_e::perceptual_quantizer},
    Fp16TransferParam {DXGI_FORMAT_AYUV, true, false, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_AYUV, true, true, fp16_transfer_e::linear},
    Fp16TransferParam {DXGI_FORMAT_Y410, true, false, fp16_transfer_e::perceptual_quantizer},
    Fp16TransferParam {DXGI_FORMAT_Y410, true, true, fp16_transfer_e::perceptual_quantizer}
  )
);

TEST(WindowsRtvClearTest, ClassifiesImmediateAndDeferredClearFormats) {
  // NV12 and P010 render target views support ClearRenderTargetView(), so the padding is
  // cleared when the conversion resources are initialized.
  EXPECT_FALSE(platf::dxgi::requires_drawn_rtv_clear(DXGI_FORMAT_NV12));
  EXPECT_FALSE(platf::dxgi::requires_drawn_rtv_clear(DXGI_FORMAT_P010));

  // Integer 4:4:4 render target views cannot be cleared with ClearRenderTargetView(), so
  // they must be initialized by drawing the black texture. This has to happen before the
  // first frame, even when that frame is blank (input-only sessions never capture content).
  EXPECT_TRUE(platf::dxgi::requires_drawn_rtv_clear(DXGI_FORMAT_AYUV));
  EXPECT_TRUE(platf::dxgi::requires_drawn_rtv_clear(DXGI_FORMAT_R16_UINT));
  EXPECT_TRUE(platf::dxgi::requires_drawn_rtv_clear(DXGI_FORMAT_Y410));
}

TEST(WindowsRtvClearTest, FirstFrameBlankConversionInImmediateClearFormatFamily) {
  // Immediate-clear formats are already cleared when the conversion resources are
  // initialized, so a blank first frame must not trigger a drawn clear or a frame draw.
  for (const auto format : {DXGI_FORMAT_NV12, DXGI_FORMAT_P010}) {
    bool rtvs_cleared = !platf::dxgi::requires_drawn_rtv_clear(format);
    EXPECT_TRUE(rtvs_cleared);

    int drawn_clear_calls = 0;
    int frame_draw_calls = 0;
    const auto result = platf::dxgi::convert_d3d_frame(
      rtvs_cleared,
      true,
      [&]() -> bool {
        ++drawn_clear_calls;
        return true;
      },
      [&]() -> int {
        ++frame_draw_calls;
        return 0;
      }
    );

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(rtvs_cleared);
    EXPECT_EQ(drawn_clear_calls, 0);
    EXPECT_EQ(frame_draw_calls, 0);
  }
}

TEST(WindowsRtvClearTest, FirstFrameBlankConversionInDeferredClearFormatFamily) {
  for (const auto format : {DXGI_FORMAT_AYUV, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_Y410}) {
    bool rtvs_cleared = !platf::dxgi::requires_drawn_rtv_clear(format);
    EXPECT_FALSE(rtvs_cleared);

    int drawn_clear_calls = 0;
    int frame_draw_calls = 0;
    auto draw_black_rtv_clear = [&]() -> bool {
      ++drawn_clear_calls;
      return true;
    };
    auto draw_captured_frame = [&]() -> int {
      ++frame_draw_calls;
      return 0;
    };

    // The first blank frame initializes the render targets by drawing the black clear.
    EXPECT_EQ(platf::dxgi::convert_d3d_frame(rtvs_cleared, true, draw_black_rtv_clear, draw_captured_frame), 0);
    EXPECT_TRUE(rtvs_cleared);
    EXPECT_EQ(drawn_clear_calls, 1);
    EXPECT_EQ(frame_draw_calls, 0);

    // A second blank frame must not re-run the drawn clear.
    EXPECT_EQ(platf::dxgi::convert_d3d_frame(rtvs_cleared, true, draw_black_rtv_clear, draw_captured_frame), 0);
    EXPECT_EQ(drawn_clear_calls, 1);
    EXPECT_EQ(frame_draw_calls, 0);

    // A subsequent non-blank frame draws the captured frame without re-clearing.
    EXPECT_EQ(platf::dxgi::convert_d3d_frame(rtvs_cleared, false, draw_black_rtv_clear, draw_captured_frame), 0);
    EXPECT_EQ(drawn_clear_calls, 1);
    EXPECT_EQ(frame_draw_calls, 1);
  }
}

TEST(WindowsRtvClearTest, DeferredClearFailureOnBlankFirstFrameFailsConversion) {
  bool rtvs_cleared = !platf::dxgi::requires_drawn_rtv_clear(DXGI_FORMAT_AYUV);
  EXPECT_FALSE(rtvs_cleared);

  bool frame_drawn = false;
  const auto result = platf::dxgi::convert_d3d_frame(
    rtvs_cleared,
    true,
    []() -> bool {
      return false;
    },
    [&]() -> int {
      frame_drawn = true;
      return 0;
    }
  );

  EXPECT_EQ(result, -1);
  EXPECT_FALSE(rtvs_cleared);
  EXPECT_FALSE(frame_drawn);
}
#endif
