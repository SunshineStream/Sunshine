/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

#include <chrono>
#include <string_view>
#include <windows.h>
#include <winnt.h>

// Windows provides a timestamp from GetBuffer() indicating exactly when audio was captured.
// Before trusting this timestamp, we check if it is compatible with our other time code.

#define MAX_QPC_TIMESTAMP_OFFSET_MS 50  ///< QPC is allowed to be +/- this many milliseconds from now().

enum qpc_status_t : int {
  QPC_PENDING,   ///< QPC offset will be checked after capturing the first audio packet
  QPC_INVALID,   ///< QPC offset exceeded MAX_QPC_TIMESTAMP_OFFSET_MS and we will generate timestamps
  QPC_VALID      ///< QPC offset fell within acceptable range and will be used
};

namespace platf {
  void
  print_status(const std::string_view &prefix, HRESULT status);
  HDESK
  syncThreadDesktop();

  int64_t
  qpc_counter();

  std::chrono::nanoseconds
  qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring
  from_utf8(const std::string &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string
  to_utf8(const std::wstring &string);
}  // namespace platf
