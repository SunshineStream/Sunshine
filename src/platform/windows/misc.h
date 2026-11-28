/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>

// platform includes
#include <Windows.h>
#include <winnt.h>

// local includes
#include "src/utility.h"

namespace platf {
  /**
   * @brief Write status details to the log.
   *
   * @param prefix Text prefix used when formatting the message.
   * @param status Native status code returned by the platform API.
   */
  void print_status(const std::string_view &prefix, HRESULT status);
  /**
   * @brief Owning handle of a desktop.
   */
  using desktop_t = util::safe_ptr_v2<std::remove_pointer_t<HDESK>, BOOL, &CloseDesktop>;

  /**
   * @brief Synchronize thread desktop.
   *
   * @return true when the thread desktop was synchronized successfully.
   */
  HDESK syncThreadDesktop();

  /**
   * @brief Build a command that delegates URL activation to the Windows shell.
   *
   * @param command_processor Absolute path to the system command processor.
   * @param url URL to pass unchanged to the shell's default protocol handler.
   * @return Command line suitable for CreateProcessW().
   */
  std::wstring make_url_launch_command(const std::wstring &command_processor, const std::wstring &url);

  /**
   * @brief Attach the calling thread to the input desktop until destroyed.
   */
  class input_desktop_guard_t {
  public:
    input_desktop_guard_t();
    ~input_desktop_guard_t();

  private:
    HDESK previous_desktop {GetThreadDesktop(GetCurrentThreadId())};
    desktop_t input_desktop {OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL)};
  };

  /**
   * @brief Read the current Windows high-resolution performance counter.
   *
   * @return Raw QPC tick value from `QueryPerformanceCounter`.
   */
  int64_t qpc_counter();

  /**
   * @brief Convert the difference between two QPC readings to nanoseconds.
   *
   * @param performance_counter1 Newer performance-counter reading.
   * @param performance_counter2 Older performance-counter reading.
   * @return Duration represented by the difference between two QPC values.
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
