/**
 * @file tests/unit/platform/linux/test_display_geometry.cpp
 * @brief Verify Linux input desktop bounds and streamed output metadata.
 */

// test includes
#include "../../../tests_common.h"

// standard includes
#include <algorithm>
#include <array>

// local includes
#include "src/platform/linux/display_geometry.h"
#include "src/video.h"

namespace {
  /**
   * @brief Geometry-only capture display, without installed graphics devices.
   */
  class geometry_display_t: public platf::display_t {
  public:
    /**
     * @brief Complete without capturing frames.
     * @return Successful capture status.
     */
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::ok;
    }

    /**
     * @brief Return no image for a geometry-only display.
     * @return Null image.
     */
    std::shared_ptr<platf::img_t> alloc_img() override {
      return nullptr;
    }

    /**
     * @brief Complete without populating an image.
     * @return Success.
     */
    int dummy_img(platf::img_t *) override {
      return 0;
    }
  };
}  // namespace

TEST(LinuxDisplayGeometryTest, EmptyAndInvalidMonitorsDoNotCreateAnInputDesktop) {
  platf::desktop_geometry_t desktop;
  EXPECT_EQ(desktop.bounds().width, 0);
  EXPECT_EQ(desktop.bounds().height, 0);
  desktop.add({.width = 0, .height = 1080});
  desktop.add({.width = 1920, .height = -1});
  EXPECT_EQ(desktop.bounds().width, 0);
  EXPECT_EQ(desktop.bounds().logical_width, 0);
}

TEST(LinuxDisplayGeometryTest, PreservesTheFirstMonitorOriginAndFallsBackToPhysicalDimensions) {
  platf::desktop_geometry_t desktop;
  desktop.add({.offset_x = 800, .offset_y = 300, .width = 1920, .height = 1080, .output_name = "DP-1"});
  const auto &bounds = desktop.bounds();
  EXPECT_EQ(bounds.offset_x, 800);
  EXPECT_EQ(bounds.offset_y, 300);
  EXPECT_EQ(bounds.width, 1920);
  EXPECT_EQ(bounds.height, 1080);
  EXPECT_EQ(bounds.logical_width, 1920);
  EXPECT_EQ(bounds.logical_height, 1080);
  EXPECT_TRUE(bounds.output_name.empty());
  desktop.add({.width = -1, .height = 1080});
  EXPECT_EQ(bounds.width, 1920);
}

TEST(LinuxDisplayGeometryTest, IncludesMonitorsLeftOfAndAboveTheOriginInEitherOrder) {
  std::array monitors {
    platf::touch_port_t {.offset_x = -1920, .offset_y = -1080, .width = 1920, .height = 1080, .logical_width = 1920, .logical_height = 1080},
    platf::touch_port_t {.offset_x = 0, .offset_y = 0, .width = 1920, .height = 1080, .logical_width = 1920, .logical_height = 1080},
  };
  for (int order = 0; order < 2; ++order) {
    platf::desktop_geometry_t desktop;
    for (const auto &monitor : monitors) {
      desktop.add(monitor);
    }
    const auto &bounds = desktop.bounds();
    EXPECT_EQ(bounds.offset_x, -1920);
    EXPECT_EQ(bounds.offset_y, -1080);
    EXPECT_EQ(bounds.width, 3840);
    EXPECT_EQ(bounds.height, 2160);
    EXPECT_EQ(bounds.logical_width, 3840);
    EXPECT_EQ(bounds.logical_height, 2160);
    std::ranges::reverse(monitors);
  }
}

TEST(LinuxDisplayGeometryTest, UsesLogicalSizesForMixedScalingAndPreservesGaps) {
  platf::desktop_geometry_t desktop;
  desktop.add({.offset_x = -1920, .offset_y = 0, .width = 3840, .height = 2160, .logical_width = 1920, .logical_height = 1080});
  desktop.add({.offset_x = 300, .offset_y = -720, .width = 2560, .height = 1440, .logical_width = 1280, .logical_height = 720});
  EXPECT_EQ(desktop.bounds().offset_x, -1920);
  EXPECT_EQ(desktop.bounds().offset_y, -720);
  EXPECT_EQ(desktop.bounds().logical_width, 3500);
  EXPECT_EQ(desktop.bounds().logical_height, 1800);
  desktop.add({.offset_x = 1580, .offset_y = 0, .width = 640, .height = 480, .logical_width = 640, .logical_height = 0});
  EXPECT_EQ(desktop.bounds().logical_width, 4140);
}

TEST(LinuxDisplayGeometryTest, PublishesDesktopOriginAndStreamedConnectorThroughTheTouchPort) {
  geometry_display_t display;
  display.width = 3840;
  display.height = 2160;
  display.logical_width = 1920;
  display.logical_height = 1080;
  display.offset_x = -1920;
  display.offset_y = -1080;
  display.output_name = "DP-1";
  platf::desktop_geometry_t desktop;
  desktop.add({.offset_x = -1920, .offset_y = -1080, .width = 3840, .height = 2160, .logical_width = 1920, .logical_height = 1080});
  desktop.add({.width = 1920, .height = 1080, .logical_width = 1920, .logical_height = 1080});
  desktop.apply(display);
  EXPECT_EQ(display.env_offset_x, -1920);
  EXPECT_EQ(display.env_offset_y, -1080);
  EXPECT_EQ(display.env_width, 3840);
  EXPECT_EQ(display.env_height, 2160);
  EXPECT_EQ(display.env_logical_width, 3840);
  EXPECT_EQ(display.env_logical_height, 2160);
  video::config_t config {};
  config.width = 2560;
  config.height = 1080;
  const auto port = video::make_port(&display, config);
  EXPECT_EQ(port.output_name, "DP-1");
  EXPECT_EQ(port.offset_x, -1920);
  EXPECT_EQ(port.offset_y, -1080);
  EXPECT_EQ(port.env_offset_x, -1920);
  EXPECT_EQ(port.env_offset_y, -1080);
  EXPECT_EQ(port.env_logical_width, 3840);
  EXPECT_EQ(port.env_logical_height, 2160);
  EXPECT_FLOAT_EQ(port.scalar_inv, 2.0F);
  EXPECT_FLOAT_EQ(port.scalar_tpcoords, 2.0F);
  EXPECT_FLOAT_EQ(port.client_offsetX, 320.0F);
}
