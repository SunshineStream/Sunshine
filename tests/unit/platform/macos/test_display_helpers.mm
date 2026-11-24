/**
 * @file tests/unit/platform/macos/test_display_helpers.mm
 * @brief Unit tests for src/platform/macos/display_helpers.*.
 */
// Only compile these tests on macOS
#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <cstdint>
  #include <cstring>
  #include <memory>

  // platform includes
  #import <CoreGraphics/CoreGraphics.h>
  #import <CoreVideo/CoreVideo.h>
  #import <VideoToolbox/VideoToolbox.h>

  // local includes
  #include <src/platform/macos/display_helpers.h>

namespace {
  /**
   * @brief Minimal display used to observe the geometry helpers.
   */
  struct test_display_t: platf::display_t {
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return nullptr;
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }
  };

  /**
   * @brief Create an IOSurface-backed pixel buffer.
   *
   * @param format Pixel format of the buffer.
   * @return Retained pixel buffer, or nullptr on failure.
   */
  CVPixelBufferRef make_pixel_buffer(OSType format) {
    NSDictionary *attrs = @{
      (NSString *) kCVPixelBufferIOSurfacePropertiesKey: @ {},
    };
    CVPixelBufferRef pixel_buffer = nullptr;
    CVPixelBufferCreate(kCFAllocatorDefault, 64, 32, format, (__bridge CFDictionaryRef) attrs, &pixel_buffer);
    return pixel_buffer;
  }

  /**
   * @brief Check that every visible sample of a plane holds the expected value.
   *
   * @tparam T Sample storage type.
   * @param pixel_buffer Locked pixel buffer.
   * @param plane Plane index.
   * @param expected Expected sample value.
   * @return `true` when every sample matches.
   */
  template<class T>
  bool plane_is(CVPixelBufferRef pixel_buffer, std::size_t plane, T expected) {
    const auto *base = static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, plane));
    const auto bytes_per_row = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane);
    // Chroma planes interleave Cb and Cr, so a row holds two samples per chroma pixel.
    const auto samples_per_row = CVPixelBufferGetWidthOfPlane(pixel_buffer, plane) * (plane == 0 ? 1 : 2);
    for (std::size_t y = 0; y < CVPixelBufferGetHeightOfPlane(pixel_buffer, plane); ++y) {
      const auto *row = reinterpret_cast<const T *>(base + y * bytes_per_row);
      for (std::size_t x = 0; x < samples_per_row; ++x) {
        if (row[x] != expected) {
          return false;
        }
      }
    }
    return true;
  }

  /**
   * @brief Convert a pixel buffer to BGRA and read back its first pixel.
   *
   * @param source Pixel buffer to convert.
   * @param bgra Receives the blue, green, red and alpha components.
   * @return `true` when the conversion succeeded.
   */
  bool first_pixel_as_bgra(CVPixelBufferRef source, std::uint8_t (&bgra)[4]) {
    VTPixelTransferSessionRef session = nullptr;
    if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &session) != noErr) {
      return false;
    }

    CVPixelBufferRef destination = make_pixel_buffer(kCVPixelFormatType_32BGRA);
    const bool ok = destination && VTPixelTransferSessionTransferImage(session, source, destination) == noErr;
    if (ok) {
      CVPixelBufferLockBaseAddress(destination, kCVPixelBufferLock_ReadOnly);
      const auto *pixel = static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddress(destination));
      std::copy_n(pixel, 4, bgra);
      CVPixelBufferUnlockBaseAddress(destination, kCVPixelBufferLock_ReadOnly);
    }

    if (destination) {
      CVPixelBufferRelease(destination);
    }
    VTPixelTransferSessionInvalidate(session);
    CFRelease(session);
    return ok;
  }

  /**
   * @brief Fill a buffer of the given format with black and check it converts to RGB black.
   *
   * @param format Pixel format under test.
   */
  void expect_converts_to_black(OSType format) {
    CVPixelBufferRef pixel_buffer = make_pixel_buffer(format);
    ASSERT_NE(pixel_buffer, nullptr);
    platf::display_helpers::fill_black(pixel_buffer);

    std::uint8_t bgra[4] {};
    ASSERT_TRUE(first_pixel_as_bgra(pixel_buffer, bgra));
    EXPECT_LE(bgra[0], 1);
    EXPECT_LE(bgra[1], 1);
    EXPECT_LE(bgra[2], 1);

    CVPixelBufferRelease(pixel_buffer);
  }
}  // namespace

TEST(DisplayHelpers, FillBlack8BitVideoRangeUsesLegalBlackAndNeutralChroma) {
  CVPixelBufferRef pixel_buffer = make_pixel_buffer(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  ASSERT_NE(pixel_buffer, nullptr);
  platf::display_helpers::fill_black(pixel_buffer);

  CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  EXPECT_TRUE(plane_is<std::uint8_t>(pixel_buffer, 0, 16));
  EXPECT_TRUE(plane_is<std::uint8_t>(pixel_buffer, 1, 128));
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

  CVPixelBufferRelease(pixel_buffer);
}

TEST(DisplayHelpers, FillBlack10BitVideoRangeUsesLegalBlackAndNeutralChroma) {
  CVPixelBufferRef pixel_buffer = make_pixel_buffer(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
  ASSERT_NE(pixel_buffer, nullptr);
  platf::display_helpers::fill_black(pixel_buffer);

  CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  EXPECT_TRUE(plane_is<std::uint16_t>(pixel_buffer, 0, 64 << 6));
  EXPECT_TRUE(plane_is<std::uint16_t>(pixel_buffer, 1, 512 << 6));
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

  CVPixelBufferRelease(pixel_buffer);
}

TEST(DisplayHelpers, FillBlackZeroFillsPackedFormats) {
  CVPixelBufferRef pixel_buffer = make_pixel_buffer(kCVPixelFormatType_32BGRA);
  ASSERT_NE(pixel_buffer, nullptr);

  CVPixelBufferLockBaseAddress(pixel_buffer, 0);
  std::memset(CVPixelBufferGetBaseAddress(pixel_buffer), 0xff, CVPixelBufferGetDataSize(pixel_buffer));
  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

  platf::display_helpers::fill_black(pixel_buffer);

  CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  const auto *base = static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddress(pixel_buffer));
  const auto bytes = CVPixelBufferGetBytesPerRow(pixel_buffer) * CVPixelBufferGetHeight(pixel_buffer);
  EXPECT_TRUE(std::all_of(base, base + bytes, [](std::uint8_t value) {
    return value == 0;
  }));
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

  CVPixelBufferRelease(pixel_buffer);
}

TEST(DisplayHelpers, FilledBuffersConvertToRgbBlack) {
  expect_converts_to_black(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  expect_converts_to_black(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
}

TEST(DisplayHelpers, ScaledDisplayKeepsPhysicalSizeAndUsesLogicalBounds) {
  test_display_t display;
  display.width = 3024;
  display.height = 1964;

  platf::display_helpers::apply_display_geometry(display, CGRectMake(0, 0, 1512, 982), CGRectMake(0, 0, 1512, 982));

  EXPECT_EQ(display.width, 3024);
  EXPECT_EQ(display.height, 1964);
  EXPECT_EQ(display.offset_x, 0);
  EXPECT_EQ(display.offset_y, 0);
  EXPECT_EQ(display.logical_width, 1512);
  EXPECT_EQ(display.logical_height, 982);
  EXPECT_EQ(display.env_logical_width, 1512);
  EXPECT_EQ(display.env_logical_height, 982);
  EXPECT_EQ(display.env_width, 1512);
  EXPECT_EQ(display.env_height, 982);
}

TEST(DisplayHelpers, SecondaryDisplayUsesItsOriginWithinTheDesktop) {
  test_display_t display;

  // A 1920x1080 display placed left of and slightly below a 1512x982 main display.
  const auto selected = CGRectMake(-1920, 120, 1920, 1080);
  const auto desktop = CGRectUnion(selected, CGRectMake(0, 0, 1512, 982));
  platf::display_helpers::apply_display_geometry(display, selected, desktop);

  EXPECT_EQ(display.offset_x, -1920);
  EXPECT_EQ(display.offset_y, 120);
  EXPECT_EQ(display.logical_width, 1920);
  EXPECT_EQ(display.logical_height, 1080);
  EXPECT_EQ(display.env_offset_x, -1920);
  EXPECT_EQ(display.env_offset_y, 0);
  EXPECT_EQ(display.env_logical_width, 3432);
  EXPECT_EQ(display.env_logical_height, 1200);
  EXPECT_EQ(display.env_width, 3432);
  EXPECT_EQ(display.env_height, 1200);
}

TEST(DisplayHelpers, InitDisplayGeometryMatchesCoreGraphics) {
  const auto display_id = CGMainDisplayID();
  const auto selected = CGDisplayBounds(display_id);
  const auto desktop = platf::display_helpers::active_desktop_bounds(display_id);

  // The desktop always contains the selected display (headless runners may report no displays).
  if (!CGRectIsEmpty(selected)) {
    EXPECT_TRUE(CGRectContainsRect(desktop, selected));
  }

  test_display_t display;
  platf::display_helpers::init_display_geometry(display, display_id);

  EXPECT_EQ(display.offset_x, static_cast<int>(selected.origin.x));
  EXPECT_EQ(display.offset_y, static_cast<int>(selected.origin.y));
  EXPECT_EQ(display.logical_width, static_cast<int>(selected.size.width));
  EXPECT_EQ(display.logical_height, static_cast<int>(selected.size.height));
  EXPECT_EQ(display.env_offset_x, static_cast<int>(desktop.origin.x));
  EXPECT_EQ(display.env_offset_y, static_cast<int>(desktop.origin.y));
  EXPECT_EQ(display.env_logical_width, static_cast<int>(desktop.size.width));
  EXPECT_EQ(display.env_logical_height, static_cast<int>(desktop.size.height));
}
#endif
