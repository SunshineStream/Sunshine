/**
 * @file src/input/platform_input.cpp
 * @brief Definitions for common platform input.
 */
#include "src/input/platform_input.h"

platf::input_t PlatformInput::_instance;

platf::input_t &
PlatformInput::getInstance() {
  return _instance;
}
