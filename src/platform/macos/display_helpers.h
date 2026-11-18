/**
 * @file src/platform/macos/display_helpers.h
 * @brief Declarations for display geometry and pixel buffer helpers shared by the macOS capture backends.
 */
#pragma once

// platform includes
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>

// local includes
#include "src/platform/common.h"

namespace platf::display_helpers {
  /**
   * @brief Fill a pixel buffer with black.
   *
   * Video-range bi-planar formats get legal black luma and neutral chroma (8-bit `420v` and
   * 10-bit `x420`); any other format is zero-filled, which is black for BGRA.
   *
   * @param pixel_buffer Pixel buffer to fill.
   */
  void fill_black(CVPixelBufferRef pixel_buffer);

  /**
   * @brief Return the union of active CoreGraphics display bounds.
   *
   * @param fallback_display Display whose bounds are used if enumeration fails.
   * @return Full active desktop bounds in CoreGraphics coordinates.
   */
  CGRect active_desktop_bounds(CGDirectDisplayID fallback_display);

  /**
   * @brief Store the selected display and desktop geometry used for input mapping.
   *
   * @param display Display to update.
   * @param selected_display_bounds Bounds of the captured display in CoreGraphics (logical) coordinates.
   * @param desktop_bounds Bounds of the full active desktop in CoreGraphics (logical) coordinates.
   */
  void apply_display_geometry(display_t &display, CGRect selected_display_bounds, CGRect desktop_bounds);

  /**
   * @brief Initialize the input-mapping geometry of a display from CoreGraphics.
   *
   * Every capture backend calls this so the mouse viewport and display scaling do not depend on
   * which backend captures the frames.
   *
   * @param display Display to update.
   * @param display_id Display being captured.
   */
  void init_display_geometry(display_t &display, CGDirectDisplayID display_id);
}  // namespace platf::display_helpers
