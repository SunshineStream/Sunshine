/**
 * @file src/platform/macos/display_helpers.mm
 * @brief Definitions for display geometry and pixel buffer helpers shared by the macOS capture backends.
 */
// standard includes
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/macos/display_helpers.h"

namespace platf::display_helpers {
  using namespace std::literals;

  namespace {
    /**
     * @brief Fill one plane of a locked pixel buffer.
     *
     * @param base Base address of the plane.
     * @param bytes Size of the plane in bytes, including row padding.
     * @param format Pixel format of the buffer.
     * @param plane Plane index; 0 is luma, 1 is interleaved chroma.
     */
    void fill_plane_black(void *base, std::size_t bytes, OSType format, std::size_t plane) {
      if (format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        std::memset(base, plane == 0 ? 16 : 128, bytes);
      } else if (format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange) {
        // x420 stores each 10-bit sample in the high bits of a 16-bit word.
        const auto value = static_cast<std::uint16_t>((plane == 0 ? 64 : 512) << 6);
        std::fill_n(static_cast<std::uint16_t *>(base), bytes / sizeof(std::uint16_t), value);
      } else {
        std::memset(base, 0, bytes);
      }
    }
  }  // namespace

  void fill_black(CVPixelBufferRef pixel_buffer) {
    CVPixelBufferLockBaseAddress(pixel_buffer, 0);

    const auto format = CVPixelBufferGetPixelFormatType(pixel_buffer);
    if (CVPixelBufferIsPlanar(pixel_buffer)) {
      for (std::size_t plane = 0; plane < CVPixelBufferGetPlaneCount(pixel_buffer); ++plane) {
        fill_plane_black(
          CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, plane),
          CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane) * CVPixelBufferGetHeightOfPlane(pixel_buffer, plane),
          format,
          plane
        );
      }
    } else {
      fill_plane_black(
        CVPixelBufferGetBaseAddress(pixel_buffer),
        CVPixelBufferGetBytesPerRow(pixel_buffer) * CVPixelBufferGetHeight(pixel_buffer),
        format,
        0
      );
    }

    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
  }

  CGRect active_desktop_bounds(CGDirectDisplayID fallback_display) {
    std::uint32_t display_count {};
    if (CGGetActiveDisplayList(0, nullptr, &display_count) != kCGErrorSuccess || display_count == 0) {
      return CGDisplayBounds(fallback_display);
    }

    std::vector<CGDirectDisplayID> displays(display_count);
    if (CGGetActiveDisplayList(display_count, displays.data(), &display_count) != kCGErrorSuccess || display_count == 0) {
      return CGDisplayBounds(fallback_display);
    }

    auto bounds = CGDisplayBounds(displays.front());
    for (std::uint32_t index = 1; index < display_count; ++index) {
      bounds = CGRectUnion(bounds, CGDisplayBounds(displays[index]));
    }
    return bounds;
  }

  void apply_display_geometry(display_t &display, CGRect selected_display_bounds, CGRect desktop_bounds) {
    display.offset_x = static_cast<int>(selected_display_bounds.origin.x);
    display.offset_y = static_cast<int>(selected_display_bounds.origin.y);
    display.logical_width = static_cast<int>(selected_display_bounds.size.width);
    display.logical_height = static_cast<int>(selected_display_bounds.size.height);
    display.env_offset_x = static_cast<int>(desktop_bounds.origin.x);
    display.env_offset_y = static_cast<int>(desktop_bounds.origin.y);
    display.env_logical_width = static_cast<int>(desktop_bounds.size.width);
    display.env_logical_height = static_cast<int>(desktop_bounds.size.height);
    display.env_width = display.env_logical_width;
    display.env_height = display.env_logical_height;
  }

  void init_display_geometry(display_t &display, CGDirectDisplayID display_id) {
    apply_display_geometry(display, CGDisplayBounds(display_id), active_desktop_bounds(display_id));

    BOOST_LOG(debug) << "Selected display bounds: "sv << display.offset_x << 'x' << display.offset_y << ' '
                     << display.logical_width << 'x' << display.logical_height;
    BOOST_LOG(debug) << "Active desktop bounds: "sv << display.env_offset_x << 'x' << display.env_offset_y << ' '
                     << display.env_logical_width << 'x' << display.env_logical_height;
  }
}  // namespace platf::display_helpers
