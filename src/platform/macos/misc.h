/**
 * @file src/platform/macos/misc.h
 * @brief Miscellaneous declarations for macOS platform.
 */
#pragma once

#include "permissions_manager.h"

#include <vector>

#include <CoreGraphics/CoreGraphics.h>

namespace platf {
  static auto permissions_manager = PermissionsManager();
}

namespace dyn {
  typedef void (*apiproc)();

  int
  load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict = true);
  void *
  handle(const std::vector<const char *> &libs);

}  // namespace dyn
