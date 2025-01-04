/**
 * @file src/platform/windows/nvprefs/nvprefs_common.cpp
 * @brief Definitions for common nvidia preferences.
 */
// local includes
#include "nvprefs_common.h"
#include "src/logging.h"

// read user override preferences from global sunshine config
#include "src/config.h"

namespace nvprefs {

  void
  info_message(const std::wstring &message) {
    BOOST_LOG(info) << "nvprefs: " << message;
  }

  void
  info_message(const std::string &message) {
    BOOST_LOG(info) << "nvprefs: " << message;
  }

  void
  error_message(const std::wstring &message) {
    BOOST_LOG(error) << "nvprefs: " << message;
  }

  void
  error_message(const std::string &message) {
    BOOST_LOG(error) << "nvprefs: " << message;
  }

  nvprefs_options
  get_nvprefs_options() {
    nvprefs_options options;
    options.opengl_vulkan_on_dxgi = config::video.nv_opengl_vulkan_on_dxgi;
    options.sunshine_high_power_mode = config::video.nv_sunshine_high_power_mode;
    options.force_disable_vsync = config::video.nv_force_disable_vsync;
    options.enable_frame_limiter = config::video.nv_enable_frame_limiter;
    return options;
  }

}  // namespace nvprefs
