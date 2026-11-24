/**
 * @file src/compat/boost_process.h
 * @brief Includes Boost.Process while isolating third-party deprecation diagnostics.
 */
#pragma once

#if defined(__APPLE__) && defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wdeprecated-declarations"

  // Boost.Process v1 still uses std::codecvt_utf8 on macOS, including in Boost 1.92.
  #include <boost/process/v1/locale.hpp>

  #pragma clang diagnostic pop
#endif

#include <boost/process/v1.hpp>
