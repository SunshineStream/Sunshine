/**
 * @file src/input/platform_input.h
 * @brief Declarations for common platform input singleton.
 */
#pragma once

#include "src/platform/common.h"

/**
 * Singleton class to hold a reference to the platform input.
 *
 * We used to have a single global instance of the platform input, declared in the input namespace,
 * but this caused issues with static initialization, while using it from many namespaces,
 * so we moved it to it's own singleton class.
 *
 * @todo Improve the whole input layer so we can easily initialize via a factory, DI, whatever and remove this.
 */
class PlatformInput {
public:
  static platf::input_t _instance;

  static platf::input_t &
  getInstance();

private:
  PlatformInput() = default;
};
