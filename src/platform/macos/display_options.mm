/**
 * @file src/platform/macos/display_options.mm
 * @brief todo
 */
#include "src/platform/common.h"
#include "src/platform/macos/av_video.h"

#include "src/config.h"
#include "src/logging.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreGraphics/CGDirectDisplay.h>

namespace fs = std::filesystem;

namespace platf {
  using namespace std::literals;

  dd::options::info_t
  display_mode(NSArray<NSScreen *>* screens, CGDirectDisplayID displayID) {
    auto id = [NSNumber numberWithUnsignedInt:displayID];
    for (NSScreen *screen in screens) {
      if (screen.deviceDescription[@"NSScreenNumber"] == id) {
        NSRect frame = screen.frame;
        auto origin = dd::origin_t {
          int(frame.origin.x),
          int(frame.origin.y)
        };
        auto resolution = dd::resolution_t {
          uint32_t(frame.size.width),
          uint32_t(frame.size.height),
          screen.backingScaleFactor
        };

        //auto minimumFramesPerSecond = uint32_t(screen.minimumFramesPerSecond);
        //double displayUpdateGranularity = screen.displayUpdateGranularity;
        auto refresh_rate = dd::refresh_rate_t {
            uint32_t(screen.maximumFramesPerSecond),
            1
        };
        auto current_mode = dd::mode_t {
          resolution,
          refresh_rate
        };
        auto settings = dd::options::current_settings_t {
          origin,
          current_mode
        };
        auto info = dd::options::info_t {
          [id stringValue].UTF8String,
          screen.localizedName.UTF8String,
          settings
        };
        return info;
      }
    }

    return {};
  }

  std::vector<dd::options::info_t>
  display_options() {
    CGDirectDisplayID active_displays[kMaxDisplays];
    uint32_t count;
    if (CGGetActiveDisplayList(kMaxDisplays, active_displays, &count) != kCGErrorSuccess) {
      return {};
    }

    std::vector<dd::options::info_t> display_options;

    display_options.reserve(count);

    auto screens = [NSScreen screens];

    for (uint32_t i = 0; i < count; i++) {
      display_options.emplace_back(
        platf::display_mode(screens, active_displays[i])
      );
    }

    return display_options;
  }
}  // namespace platf
