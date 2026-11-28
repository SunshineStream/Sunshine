/**
 * @file src/platform/windows/misc.cpp
 * @brief Miscellaneous definitions for Windows.
 */

// TODO: Revisit this override when formalizing the Windows 11 minimum API target.
#ifndef NTDDI_VERSION
  /**
   * @def NTDDI_VERSION
   * @brief Target Windows 10 APIs before Boost or Windows headers select a default.
   */
  #define NTDDI_VERSION NTDDI_WIN10
#endif

// standard includes
#include <array>
#include <csignal>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/range/algorithm/find_if.hpp>

// local includes required before platform includes
#include "src/compat/boost_process.h"

// platform includes
// prevent clang format from "optimizing" the header include order
// clang-format off
#include <dwmapi.h>
#include <iphlpapi.h>
#include <iterator>
#include <timeapi.h>
#include <UserEnv.h>
#include <WinSock2.h>
#include <Windows.h>
#include <WinUser.h>
#include <wlanapi.h>
#include <WS2tcpip.h>
#include <WtsApi32.h>
#include <sddl.h>
#include <Shlwapi.h>
// clang-format on

// local includes
#include "misc.h"
#include "nvprefs/nvprefs_interface.h"
#include "src/entry_handler.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/permissions.h"
#include "src/utility.h"
#include "utf_utils.h"

// UDP_SEND_MSG_SIZE was added in the Windows 10 20H1 SDK
#ifndef UDP_SEND_MSG_SIZE
  /**
   * @def UDP_SEND_MSG_SIZE
   * @brief Macro for UDP SEND MSG SIZE.
   */
  #define UDP_SEND_MSG_SIZE 2
#endif

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  /**
   * @def PROC_THREAD_ATTRIBUTE_JOB_LIST
   * @brief Macro for PROC THREAD ATTRIBUTE JOB LIST.
   */
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

#include <qos2.h>

#ifndef WLAN_API_MAKE_VERSION
  /**
   * @def WLAN_API_MAKE_VERSION(_major, _minor)
   * @brief Macro for WLAN API MAKE VERSION.
   */
  #define WLAN_API_MAKE_VERSION(_major, _minor) (((DWORD) (_minor)) << 16 | (_major))
#endif

#include <winternl.h>
extern "C" {
  /**
   * @brief Dynamically resolve NtSetTimerResolution from ntdll.
   *
   * @param DesiredResolution Desired resolution.
   * @param SetResolution Set resolution.
   * @param CurrentResolution Current resolution.
   * @return NTSTATUS reported by the system timer-resolution request.
   */
  NTSTATUS NTAPI NtSetTimerResolution(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
}

namespace {

  std::atomic used_nt_set_timer_resolution = false;

  bool nt_set_timer_resolution_max() {
    ULONG maximum;
    ULONG minimum;
    if (ULONG current; !NT_SUCCESS(NtQueryTimerResolution(&minimum, &maximum, &current)) || !NT_SUCCESS(NtSetTimerResolution(maximum, TRUE, &current))) {
      return false;
    }
    return true;
  }

  bool nt_set_timer_resolution_min() {
    ULONG maximum;
    ULONG minimum;
    if (ULONG current; !NT_SUCCESS(NtQueryTimerResolution(&minimum, &maximum, &current)) || !NT_SUCCESS(NtSetTimerResolution(minimum, TRUE, &current))) {
      return false;
    }
    return true;
  }

}  // namespace

namespace bp = boost::process::v1;

using namespace std::literals;

namespace platf {
  std::vector<permission_status_t> get_permission_statuses() {
    return {{"config_directory", can_access_directory(appdata()) ? "granted" : "denied", true, true}};
  }

  bool request_permission(std::string_view id) {
    // Windows does not offer a consent prompt for arbitrary directory ACLs.
    // The Web UI shows the setup steps for this known permission.
    (void) id;
    return false;
  }

  /**
   * @brief Owning pointer for `GetAdaptersAddresses` results.
   */
  using adapteraddrs_t = util::c_ptr<IP_ADAPTER_ADDRESSES>;

  bool enabled_mouse_keys = false;  ///< Tracks whether Windows Mouse Keys was enabled before Sunshine changed it.
  MOUSEKEYS previous_mouse_keys_state;  ///< Previous mouse keys state.

  HANDLE qos_handle = nullptr;  ///< QoS handle.

  decltype(QOSCreateHandle) *fn_QOSCreateHandle = nullptr;  ///< Fn QoS create handle.
  decltype(QOSAddSocketToFlow) *fn_QOSAddSocketToFlow = nullptr;  ///< Fn QoS add socket to flow.
  decltype(QOSRemoveSocketFromFlow) *fn_QOSRemoveSocketFromFlow = nullptr;  ///< Fn QoS remove socket from flow.

  HANDLE wlan_handle = nullptr;  ///< Wlan handle.

  decltype(WlanOpenHandle) *fn_WlanOpenHandle = nullptr;  ///< Fn wlan open handle.
  decltype(WlanCloseHandle) *fn_WlanCloseHandle = nullptr;  ///< Fn wlan close handle.
  decltype(WlanFreeMemory) *fn_WlanFreeMemory = nullptr;  ///< Fn wlan free memory.
  decltype(WlanEnumInterfaces) *fn_WlanEnumInterfaces = nullptr;  ///< Fn wlan enum interfaces.
  decltype(WlanSetInterface) *fn_WlanSetInterface = nullptr;  ///< Fn wlan set interface.

  std::filesystem::path appdata() {
    std::array<WCHAR, MAX_PATH> sunshine_path;
    GetModuleFileNameW(nullptr, sunshine_path.data(), sunshine_path.size());
    return std::filesystem::path {sunshine_path.data()}.remove_filename() / L"config"sv;
  }

  /**
   * @brief XDG Portal token path (unused on Windows).
   *
   * @return Path of portal_token in appdata path.
   */
  std::filesystem::path get_xdg_restore_token_path() {
    return appdata() / "portal_token";
  }

  std::string from_sockaddr(const sockaddr *const socket_address) {
    std::array<char, INET6_ADDRSTRLEN> data = {};

    if (auto family = socket_address->sa_family; family == AF_INET6) {
      inet_ntop(AF_INET6, &((const sockaddr_in6 *) socket_address)->sin6_addr, data.data(), INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((const sockaddr_in *) socket_address)->sin_addr, data.data(), INET_ADDRSTRLEN);
    }

    return std::string {data.data()};
  }

  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      port = ((const sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      port = ((const sockaddr_in *) ip_addr)->sin_port;
    }

    return {port, from_sockaddr(ip_addr)};
  }

  /**
   * @brief Read Windows adapter addresses with automatic buffer sizing.
   *
   * @return Adapter-address list populated by GetAdaptersAddresses.
   */
  adapteraddrs_t get_adapteraddrs() {
    adapteraddrs_t adapter_addresses {nullptr};
    ULONG size = 0;

    while (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapter_addresses.get(), &size) == ERROR_BUFFER_OVERFLOW) {
      adapter_addresses.reset((PIP_ADAPTER_ADDRESSES) malloc(size));
    }

    return adapter_addresses;
  }

  std::string get_mac_address(const std::string_view &address) {
    adapteraddrs_t adapter_addresses = get_adapteraddrs();
    for (auto adapter_pos = adapter_addresses.get(); adapter_pos != nullptr; adapter_pos = adapter_pos->Next) {
      for (auto addr_pos = adapter_pos->FirstUnicastAddress; addr_pos != nullptr; addr_pos = addr_pos->Next) {
        if (adapter_pos->PhysicalAddressLength != 0 && address == from_sockaddr(addr_pos->Address.lpSockaddr)) {
          std::stringstream mac_addr;
          mac_addr << std::hex;
          for (int i = 0; i < adapter_pos->PhysicalAddressLength; i++) {
            if (i > 0) {
              mac_addr << ':';
            }
            mac_addr << std::setw(2) << std::setfill('0') << (int) adapter_pos->PhysicalAddress[i];
          }
          return mac_addr.str();
        }
      }
    }
    BOOST_LOG(warning) << "Unable to find MAC address for "sv << address;
    return "00:00:00:00:00:00"s;
  }

  /**
   * @brief Synchronize thread desktop.
   */
  HDESK syncThreadDesktop() {
    // Windows can't close the desktop a thread uses, so its handle is kept until the thread moves on
    thread_local desktop_t attached_desktop;

    desktop_t hDesk {OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL)};
    if (!hDesk) {
      auto err = GetLastError();
      BOOST_LOG(error) << "Failed to Open Input Desktop [0x"sv << util::hex(err).to_string_view() << ']';

      return nullptr;
    }

    if (!SetThreadDesktop(hDesk.get())) {
      auto err = GetLastError();
      BOOST_LOG(error) << "Failed to sync desktop to thread [0x"sv << util::hex(err).to_string_view() << ']';

      return nullptr;
    }

    attached_desktop = std::move(hDesk);
    return attached_desktop.get();
  }

  input_desktop_guard_t::input_desktop_guard_t() {
    if (input_desktop && !SetThreadDesktop(input_desktop.get())) {
      input_desktop.reset();
    }
  }

  input_desktop_guard_t::~input_desktop_guard_t() {
    if (input_desktop && !SetThreadDesktop(previous_desktop)) {
      // The thread is still on the input desktop, so its handle has to stay open
      BOOST_LOG(warning) << "Failed to restore the thread desktop [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
      input_desktop.release();
    }
  }

  /**
   * @brief Write status details to the log.
   */
  void print_status(const std::string_view &prefix, HRESULT status) {
    std::array<char, 1024> err_string;

    DWORD bytes = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, status, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err_string.data(), err_string.size(), nullptr);

    BOOST_LOG(error) << prefix << ": "sv << std::string_view {err_string.data(), bytes};
  }

  /**
   * @brief Check whether a Windows access token belongs to an administrator.
   *
   * @param user_token Windows access token to inspect.
   * @return True when the inspected token has administrator privileges.
   */
  bool IsUserAdmin(HANDLE user_token) {
    WINBOOL ret;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID AdministratorsGroup;
    ret = AllocateAndInitializeSid(
      &NtAuthority,
      2,
      SECURITY_BUILTIN_DOMAIN_RID,
      DOMAIN_ALIAS_RID_ADMINS,
      0,
      0,
      0,
      0,
      0,
      0,
      &AdministratorsGroup
    );
    if (ret) {
      if (!CheckTokenMembership(user_token, AdministratorsGroup, &ret)) {
        ret = false;
        BOOST_LOG(error) << "Failed to verify token membership for administrative access: " << GetLastError();
      }
      FreeSid(AdministratorsGroup);
    } else {
      BOOST_LOG(error) << "Unable to allocate SID to check administrative access: " << GetLastError();
    }

    return ret;
  }

  /**
   * @brief Obtain the current sessions user's primary token with elevated privileges.
   * @return The user's token. If user has admin capability it will be elevated, otherwise it will be a limited token. On error, `nullptr`.
   *
   * @param elevated Whether the command should run with elevated privileges.
   */
  HANDLE retrieve_users_token(bool elevated) {
    DWORD consoleSessionId;
    HANDLE userToken;
    TOKEN_ELEVATION_TYPE elevationType;
    DWORD dwSize;

    // Get the session ID of the active console session
    consoleSessionId = WTSGetActiveConsoleSessionId();
    if (0xFFFFFFFF == consoleSessionId) {
      // If there is no active console session, log a warning and return null
      BOOST_LOG(warning) << "There isn't an active user session, therefore it is not possible to execute commands under the users profile.";
      return nullptr;
    }

    // Get the user token for the active console session
    if (!WTSQueryUserToken(consoleSessionId, &userToken)) {
      BOOST_LOG(debug) << "QueryUserToken failed, this would prevent commands from launching under the users profile.";
      return nullptr;
    }

    // We need to know if this is an elevated token or not.
    // Get the elevation type of the user token
    // Elevation - Default: User is not an admin, UAC enabled/disabled does not matter.
    // Elevation - Limited: User is an admin, has UAC enabled.
    // Elevation - Full:    User is an admin, has UAC disabled.
    if (!GetTokenInformation(userToken, TokenElevationType, &elevationType, sizeof(TOKEN_ELEVATION_TYPE), &dwSize)) {
      BOOST_LOG(debug) << "Retrieving token information failed: " << GetLastError();
      CloseHandle(userToken);
      return nullptr;
    }

    // User is currently not an administrator
    // The documentation for this scenario is conflicting, so we'll double check to see if user is actually an admin.
    if (elevated && (elevationType == TokenElevationTypeDefault && !IsUserAdmin(userToken))) {
      // We don't have to strip the token or do anything here, but let's give the user a warning so they're aware what is happening.
      BOOST_LOG(warning) << "This command requires elevation and the current user account logged in does not have administrator rights. "
                         << "For security reasons Sunshine will retain the same access level as the current user and will not elevate it.";
    }

    // User has a limited token, this means they have UAC enabled and is an Administrator
    if (elevated && elevationType == TokenElevationTypeLimited) {
      TOKEN_LINKED_TOKEN linkedToken;
      // Retrieve the administrator token that is linked to the limited token
      if (!GetTokenInformation(userToken, TokenLinkedToken, reinterpret_cast<void *>(&linkedToken), sizeof(TOKEN_LINKED_TOKEN), &dwSize)) {
        // If the retrieval failed, log an error message and return null
        BOOST_LOG(error) << "Retrieving linked token information failed: " << GetLastError();
        CloseHandle(userToken);

        // There is no scenario where this should be hit, except for an actual error.
        return nullptr;
      }

      // Since we need the elevated token, we'll replace it with their administrative token.
      CloseHandle(userToken);
      userToken = linkedToken.LinkedToken;
    }

    // We don't need to do anything for TokenElevationTypeFull users here, because they're already elevated.
    return userToken;
  }

  /**
   * @brief Merge user environment variables into a Windows environment block.
   *
   * Existing variable names retain their spelling when matched case-insensitively.
   *
   * @param env Environment variables for the child process.
   * @param shell_token Shell token.
   * @return True when the user environment block was merged into `env`.
   */
  bool merge_user_environment_block(bp::environment &env, HANDLE shell_token) {
    // Get the target user's environment block
    PVOID env_block;
    if (!CreateEnvironmentBlock(&env_block, shell_token, FALSE)) {
      return false;
    }

    // Parse the environment block and populate env
    for (auto c = (PWCHAR) env_block; *c != UNICODE_NULL; c += wcslen(c) + 1) {
      // Environment variable entries end with a null-terminator, so std::wstring() will get an entire entry.
      std::string env_tuple = utf_utils::to_utf8(std::wstring {c});
      std::string env_name = env_tuple.substr(0, env_tuple.find('='));
      std::string env_val = env_tuple.substr(env_tuple.find('=') + 1);

      // Perform a case-insensitive search to see if this variable name already exists
      if (auto itr = boost::range::find_if(env, [&env_name](const auto &e) {
            return boost::iequals(e.get_name(), env_name);
          });
          itr != env.end()) {
        // Use this existing name if it is already present to ensure we merge properly
        env_name = itr->get_name();
      }

      // For the PATH variable, we will merge the values together
      if (boost::iequals(env_name, "PATH")) {
        env[env_name] = env_val + ";" + env[env_name].to_string();
      } else {
        // Other variables will be superseded by those in the user's environment block
        env[env_name] = env_val;
      }
    }

    DestroyEnvironmentBlock(env_block);
    return true;
  }

  /**
   * @brief Check if the current process is running with system-level privileges.
   * @return `true` if the current process has system-level privileges, `false` otherwise.
   */
  bool is_running_as_system() {
    BOOL ret;
    PSID SystemSid;
    DWORD dwSize = SECURITY_MAX_SID_SIZE;

    // Allocate memory for the SID structure
    SystemSid = LocalAlloc(LMEM_FIXED, dwSize);
    if (SystemSid == nullptr) {
      BOOST_LOG(error) << "Failed to allocate memory for the SID structure: " << GetLastError();
      return false;
    }

    // Create a SID for the local system account
    ret = CreateWellKnownSid(WinLocalSystemSid, nullptr, SystemSid, &dwSize);
    if (ret) {
      // Check if the current process token contains this SID
      if (!CheckTokenMembership(nullptr, SystemSid, &ret)) {
        BOOST_LOG(error) << "Failed to check token membership: " << GetLastError();
        ret = false;
      }
    } else {
      BOOST_LOG(error) << "Failed to create a SID for the local system account. This may happen if the system is out of memory or if the SID buffer is too small: " << GetLastError();
    }

    // Free the memory allocated for the SID structure
    LocalFree(SystemSid);
    return ret;
  }

  // Note: This does NOT append a null terminator
  /**
   * @brief Append a null-terminated string to a Windows environment block.
   *
   * @param env_block Env block.
   * @param offset Byte offset used when converting from the wide string buffer.
   * @param wstr Wide-character string being converted to UTF-8.
   */
  void append_string_to_environment_block(wchar_t *env_block, int &offset, const std::wstring &wstr) {
    std::memcpy(&env_block[offset], wstr.data(), wstr.length() * sizeof(wchar_t));
    offset += wstr.length();
  }

  /**
   * @brief Create environment block.
   *
   * @param env Environment variables for the child process.
   * @return Created environment block object or status.
   */
  std::wstring create_environment_block(const bp::environment &env) {
    int size = 0;
    for (const auto &entry : env) {
      auto name = entry.get_name();
      auto value = entry.to_string();
      size += utf_utils::from_utf8(name).length() + 1 /* L'=' */ + utf_utils::from_utf8(value).length() + 1 /* L'\0' */;
    }

    size += 1 /* L'\0' */;

    std::vector<wchar_t> env_block(size);
    int offset = 0;
    for (const auto &entry : env) {
      auto name = entry.get_name();
      auto value = entry.to_string();

      // Construct the NAME=VAL\0 string
      append_string_to_environment_block(env_block.data(), offset, utf_utils::from_utf8(name));
      env_block[offset] = L'=';
      offset++;
      append_string_to_environment_block(env_block.data(), offset, utf_utils::from_utf8(value));
      env_block[offset] = L'\0';
      offset++;
    }

    // Append a final null terminator
    env_block[offset] = L'\0';
    offset++;

    return std::wstring(env_block.data(), offset);
  }

  /**
   * @brief Allocate and initialize a Windows process-thread attribute list.
   *
   * @param attribute_count Attribute count.
   * @return Initialized attribute list, or nullptr when allocation fails.
   */
  LPPROC_THREAD_ATTRIBUTE_LIST allocate_proc_thread_attr_list(DWORD attribute_count) {
    SIZE_T size;
    InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);

    auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
    if (list == nullptr) {
      return nullptr;
    }

    if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
      HeapFree(GetProcessHeap(), 0, list);
      return nullptr;
    }

    return list;
  }

  /**
   * @brief Release proc thread attr list resources.
   *
   * @param list Multi-string list returned by the Windows API.
   */
  void free_proc_thread_attr_list(LPPROC_THREAD_ATTRIBUTE_LIST list) {
    DeleteProcThreadAttributeList(list);
    HeapFree(GetProcessHeap(), 0, list);
  }

  /**
   * @brief Create a `bp::child` object from the results of launching a process.
   * @param process_launched A boolean indicating if the launch was successful.
   * @param cmd The command that was used to launch the process.
   * @param ec A reference to an `std::error_code` object that will store any error that occurred during the launch.
   * @param process_info A reference to a `PROCESS_INFORMATION` structure that contains information about the new process.
   * @return A `bp::child` object representing the new process, or an empty `bp::child` object if the launch failed.
   */
  bp::child create_boost_child_from_results(bool process_launched, const std::string &cmd, std::error_code &ec, PROCESS_INFORMATION &process_info) {
    // Use RAII to ensure the process is closed when we're done with it, even if there was an error.
    auto close_process_handles = util::fail_guard([process_launched, process_info]() {
      if (process_launched) {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
      }
    });

    if (ec) {
      // If there was an error, return an empty bp::child object
      return bp::child();
    }

    if (process_launched) {
      // If the launch was successful, create a new bp::child object representing the new process
      auto child = bp::child((bp::pid_t) process_info.dwProcessId);
      BOOST_LOG(info) << cmd << " running with PID "sv << child.id();
      return child;
    } else {
      auto winerror = GetLastError();
      BOOST_LOG(error) << "Failed to launch process: "sv << winerror;
      ec = std::make_error_code(std::errc::invalid_argument);
      // We must NOT attach the failed process here, since this case can potentially be induced by ACL
      // manipulation (denying yourself execute permission) to cause an escalation of privilege.
      // So to protect ourselves against that, we'll return an empty child process instead.
      return bp::child();
    }
  }

  /**
   * @brief Impersonate the current user and invoke the callback function.
   * @param user_token A handle to the user's token that was obtained from the shell.
   * @param callback A function that will be executed while impersonating the user.
   * @return Object that will store any error that occurred during the impersonation
   */
  std::error_code impersonate_current_user(HANDLE user_token, std::function<void()> callback) {
    std::error_code ec;
    // Impersonate the user when launching the process. This will ensure that appropriate access
    // checks are done against the user token, not our SYSTEM token. It will also allow network
    // shares and mapped network drives to be used as launch targets, since those credentials
    // are stored per-user.
    if (!ImpersonateLoggedOnUser(user_token)) {
      auto winerror = GetLastError();
      // Log the failure of impersonating the user and its error code
      BOOST_LOG(error) << "Failed to impersonate user: "sv << winerror;
      ec = std::make_error_code(std::errc::permission_denied);
      return ec;
    }

    // Execute the callback function while impersonating the user
    callback();

    // End impersonation of the logged on user. If this fails (which is extremely unlikely),
    // we will be running with an unknown user token. The only safe thing to do in that case
    // is terminate ourselves.
    if (!RevertToSelf()) {
      auto winerror = GetLastError();
      // Log the failure of reverting to self and its error code
      BOOST_LOG(fatal) << "Failed to revert to self after impersonation: "sv << winerror;
      DebugBreak();
    }

    return ec;
  }

  /**
   * @brief Create a `STARTUPINFOEXW` structure for launching a process.
   * @param file A pointer to a `FILE` object that will be used as the standard output and error for the new process, or null if not needed.
   * @param job A job object handle to insert the new process into. This pointer must remain valid for the life of this startup info!
   * @param ec A reference to a `std::error_code` object that will store any error that occurred during the creation of the structure.
   * @return A structure that contains information about how to launch the new process.
   */
  STARTUPINFOEXW create_startup_info(FILE *file, HANDLE *job, std::error_code &ec) {
    // Initialize a zeroed-out STARTUPINFOEXW structure and set its size
    STARTUPINFOEXW startup_info = {};
    startup_info.StartupInfo.cb = sizeof(startup_info);

    // Allocate a process attribute list with space for 2 elements
    startup_info.lpAttributeList = allocate_proc_thread_attr_list(2);
    if (startup_info.lpAttributeList == nullptr) {
      // If the allocation failed, set ec to an appropriate error code and return the structure
      ec = std::make_error_code(std::errc::not_enough_memory);
      return startup_info;
    }

    if (file) {
      // If a file was provided, get its handle and use it as the standard output and error for the new process
      auto log_file_handle = (HANDLE) _get_osfhandle(_fileno(file));

      // Populate std handles if the caller gave us a log file to use
      startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
      startup_info.StartupInfo.hStdInput = nullptr;
      startup_info.StartupInfo.hStdOutput = log_file_handle;
      startup_info.StartupInfo.hStdError = log_file_handle;

      // Allow the log file handle to be inherited by the child process (without inheriting all of
      // our inheritable handles, such as our own log file handle created by SunshineSvc).
      //
      // Note: The value we point to here must be valid for the lifetime of the attribute list,
      // so we need to point into the STARTUPINFO instead of our log_file_variable on the stack.
      UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &startup_info.StartupInfo.hStdOutput, sizeof(startup_info.StartupInfo.hStdOutput), nullptr, nullptr);
    }

    if (job) {
      // Atomically insert the new process into the specified job.
      //
      // Note: The value we point to here must be valid for the lifetime of the attribute list,
      // so we take a HANDLE* instead of just a HANDLE to use the caller's stack storage.
      UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, job, sizeof(*job), nullptr, nullptr);
    }

    return startup_info;
  }

  /**
   * @brief This function overrides HKEY_CURRENT_USER and HKEY_CLASSES_ROOT using the provided token.
   * @param token The primary token identifying the user to use, or `nullptr` to restore original keys.
   * @return `true` if the override or restore operation was successful.
   */
  bool override_per_user_predefined_keys(HANDLE token) {
    HKEY user_classes_root = nullptr;
    if (token) {
      auto err = RegOpenUserClassesRoot(token, 0, GENERIC_ALL, &user_classes_root);
      if (err != ERROR_SUCCESS) {
        BOOST_LOG(error) << "Failed to open classes root for target user: "sv << err;
        return false;
      }
    }
    auto close_classes_root = util::fail_guard([user_classes_root]() {
      if (user_classes_root) {
        RegCloseKey(user_classes_root);
      }
    });

    HKEY user_key = nullptr;
    if (token) {
      impersonate_current_user(token, [&]() {
        // RegOpenCurrentUser() doesn't take a token. It assumes we're impersonating the desired user.
        auto err = RegOpenCurrentUser(GENERIC_ALL, &user_key);
        if (err != ERROR_SUCCESS) {
          BOOST_LOG(error) << "Failed to open user key for target user: "sv << err;
          user_key = nullptr;
        }
      });
      if (!user_key) {
        return false;
      }
    }
    auto close_user = util::fail_guard([user_key]() {
      if (user_key) {
        RegCloseKey(user_key);
      }
    });

    auto err = RegOverridePredefKey(HKEY_CLASSES_ROOT, user_classes_root);
    if (err != ERROR_SUCCESS) {
      BOOST_LOG(error) << "Failed to override HKEY_CLASSES_ROOT: "sv << err;
      return false;
    }

    err = RegOverridePredefKey(HKEY_CURRENT_USER, user_key);
    if (err != ERROR_SUCCESS) {
      BOOST_LOG(error) << "Failed to override HKEY_CURRENT_USER: "sv << err;
      RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
      return false;
    }

    return true;
  }

  /**
   * @brief Quote/escape an argument according to the Windows parsing convention.
   * @param argument The raw argument to process.
   * @return An argument string suitable for use by CreateProcess().
   */
  std::wstring escape_argument(const std::wstring &argument) {
    // If there are no characters requiring quoting/escaping, we're done
    if (argument.find_first_of(L" \t\n\v\"") == argument.npos) {
      return argument;
    }

    // The algorithm implemented here comes from a MSDN blog post:
    // https://web.archive.org/web/20120201194949/http://blogs.msdn.com/b/twistylittlepassagesallalike/archive/2011/04/23/everyone-quotes-arguments-the-wrong-way.aspx
    std::wstring escaped_arg;
    escaped_arg.push_back(L'"');
    for (auto it = argument.begin();; it++) {
      auto backslash_count = 0U;
      while (it != argument.end() && *it == L'\\') {
        it++;
        backslash_count++;
      }

      if (it == argument.end()) {
        escaped_arg.append(backslash_count * 2, L'\\');
        break;
      } else if (*it == L'"') {
        escaped_arg.append(backslash_count * 2 + 1, L'\\');
      } else {
        escaped_arg.append(backslash_count, L'\\');
      }

      escaped_arg.push_back(*it);
    }
    escaped_arg.push_back(L'"');
    return escaped_arg;
  }

  /**
   * @brief Escape an argument according to cmd's parsing convention.
   * @param argument An argument already escaped by `escape_argument()`.
   * @return An argument string suitable for use by cmd.exe.
   */
  std::wstring escape_argument_for_cmd(const std::wstring &argument) {
    // Start with the original string and modify from there
    std::wstring escaped_arg = argument;

    // Look for the next cmd metacharacter
    size_t match_pos = 0;
    while ((match_pos = escaped_arg.find_first_of(L"()%!^\"<>&|", match_pos)) != std::wstring::npos) {
      // Insert an escape character and skip past the match
      escaped_arg.insert(match_pos, 1, L'^');
      match_pos += 2;
    }

    return escaped_arg;
  }

  /**
   * @brief Resolve the given raw command into a proper command string for CreateProcess().
   * @details This converts URLs and non-executable file paths into a runnable command like ShellExecute().
   * @param raw_cmd The raw command provided by the user.
   * @param working_dir The working directory for the new process.
   * @param token The user token currently being impersonated or `nullptr` if running as ourselves.
   * @param creation_flags The creation flags for CreateProcess(), which may be modified by this function.
   * @return A command string suitable for use by CreateProcess().
   */
  std::wstring resolve_command_string(const std::string &raw_cmd, const std::wstring &working_dir, HANDLE token, DWORD &creation_flags) {
    std::wstring raw_cmd_w = utf_utils::from_utf8(raw_cmd);

    // First, convert the given command into parts so we can get the executable/file/URL without parameters
    auto raw_cmd_parts = boost::program_options::split_winmain(raw_cmd_w);
    if (raw_cmd_parts.empty()) {
      // This is highly unexpected, but we'll just return the raw string and hope for the best.
      BOOST_LOG(warning) << "Failed to split command string: "sv << raw_cmd;
      return utf_utils::from_utf8(raw_cmd);
    }

    auto raw_target = raw_cmd_parts.at(0);
    std::wstring lookup_string;
    HRESULT res;

    if (PathIsURLW(raw_target.c_str())) {
      std::array<WCHAR, 128> scheme;

      DWORD out_len = scheme.size();
      res = UrlGetPartW(raw_target.c_str(), scheme.data(), &out_len, URL_PART_SCHEME, 0);
      if (res != S_OK) {
        BOOST_LOG(warning) << "Failed to extract URL scheme from URL: "sv << raw_target << " ["sv << util::hex(res).to_string_view() << ']';
        return utf_utils::from_utf8(raw_cmd);
      }

      // If the target is a URL, the class is found using the URL scheme (prior to and not including the ':')
      lookup_string = scheme.data();
    } else {
      // If the target is not a URL, assume it's a regular file path
      auto extension = PathFindExtensionW(raw_target.c_str());
      if (extension == nullptr || *extension == 0) {
        // If the file has no extension, assume it's a command and allow CreateProcess()
        // to try to find it via PATH
        return utf_utils::from_utf8(raw_cmd);
      } else if (boost::iequals(extension, L".exe")) {
        // If the file has an .exe extension, we will bypass the resolution here and
        // directly pass the unmodified command string to CreateProcess(). The argument
        // escaping rules are subtly different between CreateProcess() and ShellExecute(),
        // and we want to preserve backwards compatibility with older configs.
        return utf_utils::from_utf8(raw_cmd);
      }

      // For regular files, the class is found using the file extension (including the dot)
      lookup_string = extension;
    }

    std::array<WCHAR, MAX_PATH> shell_command_string;
    bool needs_cmd_escaping = false;
    {
      // Overriding these predefined keys affects process-wide state, so serialize all calls
      // to ensure the handle state is consistent while we perform the command query.
      static std::mutex per_user_key_mutex;
      auto lg = std::scoped_lock(per_user_key_mutex);

      // Override HKEY_CLASSES_ROOT and HKEY_CURRENT_USER to ensure we query the correct class info
      if (!override_per_user_predefined_keys(token)) {
        return utf_utils::from_utf8(raw_cmd);
      }

      // Find the command string for the specified class
      DWORD out_len = shell_command_string.size();
      res = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, lookup_string.c_str(), L"open", shell_command_string.data(), &out_len);

      // In some cases (UWP apps), we might not have a command for this target. If that happens,
      // we'll have to launch via cmd.exe. This prevents proper job tracking, but that was already
      // broken for UWP apps anyway due to how they are started by Windows. Even 'start /wait'
      // doesn't work properly for UWP, so really no termination tracking seems to work at all.
      //
      // FIXME: Maybe we can improve this in the future.
      if (res == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)) {
        BOOST_LOG(warning) << "Using trampoline to handle target: "sv << raw_cmd;
        std::wcscpy(shell_command_string.data(), L"cmd.exe /c start \"\" /wait \"%1\" %*");
        needs_cmd_escaping = true;

        // We must suppress the console window that would otherwise appear when starting cmd.exe.
        creation_flags &= ~CREATE_NEW_CONSOLE;
        creation_flags |= CREATE_NO_WINDOW;

        res = S_OK;
      }

      // Reset per-user keys back to the original value
      override_per_user_predefined_keys(nullptr);
    }

    if (res != S_OK) {
      BOOST_LOG(warning) << "Failed to query command string for raw command: "sv << raw_cmd << " ["sv << util::hex(res).to_string_view() << ']';
      return utf_utils::from_utf8(raw_cmd);
    }

    // Finally, construct the real command string that will be passed into CreateProcess().
    // We support common substitutions (%*, %1, %2, %L, %W, %V, etc), but there are other
    // uncommon ones that are unsupported here.
    //
    // https://web.archive.org/web/20111002101214/http://msdn.microsoft.com/en-us/library/windows/desktop/cc144101(v=vs.85).aspx
    std::wstring cmd_string {shell_command_string.data()};
    size_t match_pos = 0;
    while ((match_pos = cmd_string.find_first_of(L'%', match_pos)) != std::wstring::npos) {
      std::wstring match_replacement;

      // If no additional character exists after the match, the dangling '%' is stripped
      if (match_pos + 1 == cmd_string.size()) {
        cmd_string.erase(match_pos, 1);
        break;
      }

      // Shell command replacements are strictly '%' followed by a single non-'%' character
      auto next_char = std::tolower(cmd_string.at(match_pos + 1));
      switch (next_char) {
        // Escape character
        case L'%':
          match_replacement = L'%';
          break;

        // Argument replacements
        case L'0':
        case L'1':
        case L'2':
        case L'3':
        case L'4':
        case L'5':
        case L'6':
        case L'7':
        case L'8':
        case L'9':
          {
            // Arguments numbers are 1-based, except for %0 which is equivalent to %1
            int index = next_char - L'0';
            if (next_char != L'0') {
              index--;
            }

            // Replace with the matching argument, or nothing if the index is invalid
            if (index < raw_cmd_parts.size()) {
              match_replacement = raw_cmd_parts.at(index);
            }
            break;
          }

        // All arguments following the target
        case L'*':
          for (int i = 1; i < raw_cmd_parts.size(); i++) {
            // Insert a space before arguments after the first one
            if (i > 1) {
              match_replacement += L' ';
            }

            // Argument escaping applies only to %*, not the single substitutions like %2
            auto escaped_argument = escape_argument(raw_cmd_parts.at(i));
            if (needs_cmd_escaping) {
              // If we're using the cmd.exe trampoline, we'll need to add additional escaping
              escaped_argument = escape_argument_for_cmd(escaped_argument);
            }
            match_replacement += escaped_argument;
          }
          break;

        // Long file path of target
        case L'l':
        case L'd':
        case L'v':
          {
            std::array<WCHAR, MAX_PATH> path;
            std::array<PCWCHAR, 2> other_dirs {working_dir.c_str(), nullptr};

            // PathFindOnPath() is a little gross because it uses the same
            // buffer for input and output, so we need to copy our input
            // into the path array.
            std::wcsncpy(path.data(), raw_target.c_str(), path.size());
            if (path[path.size() - 1] != 0) {
              // The path was so long it was truncated by this copy. We'll
              // assume it was an absolute path (likely) and use it unmodified.
              match_replacement = raw_target;
            }
            // See if we can find the path on our search path or working directory
            else if (PathFindOnPathW(path.data(), other_dirs.data())) {
              match_replacement = std::wstring {path.data()};
            } else {
              // We couldn't find the target, so we'll just hope for the best
              match_replacement = raw_target;
            }
            break;
          }

        // Working directory
        case L'w':
          match_replacement = working_dir;
          break;

        default:
          BOOST_LOG(warning) << "Unsupported argument replacement: %%" << next_char;
          break;
      }

      // Replace the % and following character with the match replacement
      cmd_string.replace(match_pos, 2, match_replacement);

      // Skip beyond the match replacement itself to prevent recursive replacement
      match_pos += match_replacement.size();
    }

    BOOST_LOG(info) << "Resolved user-provided command '"sv << raw_cmd << "' to '"sv << cmd_string << '\'';
    return cmd_string;
  }

  /**
   * @brief Run a command on the users profile.
   *
   * Launches a child process as the user, using the current user's environment and a specific working directory.
   *
   * @param elevated Specify whether to elevate the process.
   * @param interactive Specify whether this will run in a window or hidden.
   * @param cmd The command to run.
   * @param working_dir The working directory for the new process.
   * @param env The environment variables to use for the new process.
   * @param file A file object to redirect the child process's output to (may be `nullptr`).
   * @param ec An error code, set to indicate any errors that occur during the launch process.
   * @param group A pointer to a `bp::group` object to which the new process should belong (may be `nullptr`).
   * @return A `bp::child` object representing the new process, or an empty `bp::child` object if the launch fails.
   */
  bp::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    std::wstring start_dir = utf_utils::from_utf8(working_dir.string());
    HANDLE job = group ? group->native_handle() : nullptr;
    STARTUPINFOEXW startup_info = create_startup_info(file, job ? &job : nullptr, ec);
    PROCESS_INFORMATION process_info;

    // Clone the environment to create a local copy. Boost.Process (bp) shares the environment with all spawned processes.
    // Since we're going to modify the 'env' variable by merging user-specific environment variables into it,
    // we make a clone to prevent side effects to the shared environment.
    bp::environment cloned_env = env;

    if (ec) {
      // In the event that startup_info failed, return a blank child process.
      return bp::child();
    }

    // Use RAII to ensure the attribute list is freed when we're done with it
    auto attr_list_free = util::fail_guard([list = startup_info.lpAttributeList]() {
      free_proc_thread_attr_list(list);
    });

    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;

    // Create a new console for interactive processes and use no console for non-interactive processes
    creation_flags |= interactive ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;

    // Find the PATH variable in our environment block using a case-insensitive search
    auto sunshine_wenv = boost::this_process::wenvironment();
    std::wstring path_var_name {L"PATH"};
    std::wstring old_path_val;
    auto itr = boost::range::find_if(std::as_const(sunshine_wenv), [&path_var_name](const auto &e) {
      return boost::iequals(e.get_name(), path_var_name);
    });
    if (itr != sunshine_wenv.cend()) {
      // Use the existing variable if it exists, since Boost treats these as case-sensitive.
      path_var_name = itr->get_name();
      old_path_val = sunshine_wenv[path_var_name].to_string();
    }

    // Temporarily prepend the specified working directory to PATH to ensure CreateProcess()
    // will (preferentially) find binaries that reside in the working directory.
    sunshine_wenv[path_var_name].assign(start_dir + L";" + old_path_val);

    // Restore the old PATH value for our process when we're done here
    auto restore_path = util::fail_guard([&old_path_val, &sunshine_wenv, &path_var_name]() {
      if (old_path_val.empty()) {
        sunshine_wenv[path_var_name].clear();
      } else {
        sunshine_wenv[path_var_name].assign(old_path_val);
      }
    });

    BOOL ret;
    if (is_running_as_system()) {
      // Duplicate the current user's token
      HANDLE user_token = retrieve_users_token(elevated);
      if (!user_token) {
        // Fail the launch rather than risking launching with Sunshine's permissions unmodified.
        ec = std::make_error_code(std::errc::permission_denied);
        return bp::child();
      }

      // Use RAII to ensure the shell token is closed when we're done with it
      auto token_close = util::fail_guard([user_token]() {
        CloseHandle(user_token);
      });

      // Populate env with user-specific environment variables
      if (!merge_user_environment_block(cloned_env, user_token)) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return bp::child();
      }

      // Open the process as the current user account, elevation is handled in the token itself.
      ec = impersonate_current_user(user_token, [&]() {
        std::wstring env_block = create_environment_block(cloned_env);
        std::wstring wcmd = resolve_command_string(cmd, start_dir, user_token, creation_flags);
        ret = CreateProcessAsUserW(user_token, nullptr, (LPWSTR) wcmd.c_str(), nullptr, nullptr, !!(startup_info.StartupInfo.dwFlags & STARTF_USESTDHANDLES), creation_flags, env_block.data(), start_dir.empty() ? nullptr : start_dir.c_str(), (LPSTARTUPINFOW) &startup_info, &process_info);
      });
    }
    // Otherwise, launch the process using CreateProcessW()
    // This will inherit the elevation of whatever the user launched Sunshine with.
    else {
      // Open our current token to resolve environment variables
      HANDLE process_token;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token)) {
        ec = std::make_error_code(std::errc::permission_denied);
        return bp::child();
      }
      auto token_close = util::fail_guard([process_token]() {
        CloseHandle(process_token);
      });

      // Populate env with user-specific environment variables
      if (!merge_user_environment_block(cloned_env, process_token)) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return bp::child();
      }

      std::wstring env_block = create_environment_block(cloned_env);
      std::wstring wcmd = resolve_command_string(cmd, start_dir, nullptr, creation_flags);
      ret = CreateProcessW(nullptr, (LPWSTR) wcmd.c_str(), nullptr, nullptr, !!(startup_info.StartupInfo.dwFlags & STARTF_USESTDHANDLES), creation_flags, env_block.data(), start_dir.empty() ? nullptr : start_dir.c_str(), (LPSTARTUPINFOW) &startup_info, &process_info);
    }

    // Use the results of the launch to create a bp::child object
    return create_boost_child_from_results(ret, cmd, ec, process_info);
  }

  std::wstring make_url_launch_command(const std::wstring &command_processor, const std::wstring &url) {
    return std::format(LR"({} /d /v:off /c start "" {})", escape_argument(command_processor), escape_argument_for_cmd(escape_argument(url)));
  }

  /**
   * @brief Open a URL through the Windows shell in the user's session.
   * @details The command processor's start command preserves shell activation for packaged browsers.
   */
  void open_url(const std::string &url) {
    std::array<wchar_t, MAX_PATH> system_directory;
    const auto length = GetSystemDirectoryW(system_directory.data(), system_directory.size());
    if (!length || length >= system_directory.size()) {
      BOOST_LOG(warning) << "Couldn't locate Windows command processor to open url ["sv << url << ']';
      return;
    }
    // Use the system command processor and disable AutoRun and delayed environment expansion.
    // start performs shell activation in the user's process context, including MSIX activation.
    const auto command = utf_utils::to_utf8(make_url_launch_command(std::wstring {system_directory.data(), length} + L"\\cmd.exe", utf_utils::from_utf8(url)));
    boost::process::v1::environment _env = boost::this_process::environment();
    auto working_dir = boost::filesystem::path();
    std::error_code ec;

    auto child = run_command(false, false, command, working_dir, _env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't open url ["sv << url << "]: System: "sv << ec.message();
    } else {
      BOOST_LOG(info) << "Opening url ["sv << url << "]"sv;
      child.detach();
    }
  }

  /**
   * @brief Set the calling thread's Windows scheduling priority.
   */
  void adjust_thread_priority(thread_priority_e priority) {
    int win32_priority;

    switch (priority) {
      using enum thread_priority_e;
      case low:
        win32_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
      case normal:
        win32_priority = THREAD_PRIORITY_NORMAL;
        break;
      case high:
        win32_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
      case critical:
        win32_priority = THREAD_PRIORITY_HIGHEST;
        break;
      default:
        BOOST_LOG(error) << "Unknown thread priority: "sv << std::to_underlying(priority);
        return;
    }

    if (!SetThreadPriority(GetCurrentThread(), win32_priority)) {
      auto winerr = GetLastError();
      BOOST_LOG(warning) << "Unable to set thread priority to "sv << win32_priority << ": "sv << winerr;
    }
  }

  void set_thread_name(std::string_view name) {
    std::wstring wname = utf_utils::from_utf8(std::string {name});
    HRESULT hr = SetThreadDescription(GetCurrentThread(), wname.c_str());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "SetThreadDescription failed: " << hr;
    }
  }

  void streaming_will_start() {
    static std::once_flag load_wlanapi_once_flag;
    std::call_once(load_wlanapi_once_flag, []() {
      // wlanapi.dll is not installed by default on Windows Server, so we load it dynamically
      HMODULE wlanapi = LoadLibraryExA("wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!wlanapi) {
        BOOST_LOG(debug) << "wlanapi.dll is not available on this OS"sv;
        return;
      }

      fn_WlanOpenHandle = (decltype(fn_WlanOpenHandle)) GetProcAddress(wlanapi, "WlanOpenHandle");
      fn_WlanCloseHandle = (decltype(fn_WlanCloseHandle)) GetProcAddress(wlanapi, "WlanCloseHandle");
      fn_WlanFreeMemory = (decltype(fn_WlanFreeMemory)) GetProcAddress(wlanapi, "WlanFreeMemory");
      fn_WlanEnumInterfaces = (decltype(fn_WlanEnumInterfaces)) GetProcAddress(wlanapi, "WlanEnumInterfaces");
      fn_WlanSetInterface = (decltype(fn_WlanSetInterface)) GetProcAddress(wlanapi, "WlanSetInterface");

      if (!fn_WlanOpenHandle || !fn_WlanCloseHandle || !fn_WlanFreeMemory || !fn_WlanEnumInterfaces || !fn_WlanSetInterface) {
        BOOST_LOG(error) << "wlanapi.dll is missing exports?"sv;

        fn_WlanOpenHandle = nullptr;
        fn_WlanCloseHandle = nullptr;
        fn_WlanFreeMemory = nullptr;
        fn_WlanEnumInterfaces = nullptr;
        fn_WlanSetInterface = nullptr;

        FreeLibrary(wlanapi);
        return;
      }
    });

    // Enable MMCSS scheduling for DWM
    DwmEnableMMCSS(true);

    // Reduce timer period to 0.5ms
    if (nt_set_timer_resolution_max()) {
      used_nt_set_timer_resolution = true;
    } else {
      BOOST_LOG(error) << "NtSetTimerResolution() failed, falling back to timeBeginPeriod()";
      timeBeginPeriod(1);
      used_nt_set_timer_resolution = false;
    }

    // Promote ourselves to high priority class
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Modify NVIDIA control panel settings again, in case they have been changed externally since sunshine launch
    if (nvprefs_instance.load()) {
      if (!nvprefs_instance.owning_undo_file()) {
        nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      }
      nvprefs_instance.modify_application_profile();
      nvprefs_instance.modify_global_profile();
      nvprefs_instance.unload();
    }

    // Enable low latency mode on all connected WLAN NICs if wlanapi.dll is available
    if (fn_WlanOpenHandle) {
      DWORD negotiated_version;

      if (fn_WlanOpenHandle(WLAN_API_MAKE_VERSION(2, 0), nullptr, &negotiated_version, &wlan_handle) == ERROR_SUCCESS) {
        PWLAN_INTERFACE_INFO_LIST wlan_interface_list;

        if (fn_WlanEnumInterfaces(wlan_handle, nullptr, &wlan_interface_list) == ERROR_SUCCESS) {
          for (DWORD i = 0; i < wlan_interface_list->dwNumberOfItems; i++) {
            if (wlan_interface_list->InterfaceInfo[i].isState == wlan_interface_state_connected) {
              // Enable media streaming mode for 802.11 wireless interfaces to reduce latency and
              // unnecessary background scanning operations that cause packet loss and jitter.
              //
              // https://docs.microsoft.com/en-us/windows-hardware/drivers/network/oid-wdi-set-connection-quality
              // https://docs.microsoft.com/en-us/previous-versions/windows/hardware/wireless/native-802-11-media-streaming
              BOOL value = TRUE;
              auto wlan_status = fn_WlanSetInterface(wlan_handle, &wlan_interface_list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_media_streaming_mode, sizeof(value), &value, nullptr);
              if (wlan_status == ERROR_SUCCESS) {
                BOOST_LOG(info) << "WLAN interface "sv << i << " is now in low latency mode"sv;
              }
            }
          }

          fn_WlanFreeMemory(wlan_interface_list);
        } else {
          fn_WlanCloseHandle(wlan_handle, nullptr);
          wlan_handle = nullptr;
        }
      }
    }
    enable_mouse_keys();
  }

  void enable_mouse_keys() {
    // If there is no mouse connected, enable Mouse Keys to force the cursor to appear
    if (!GetSystemMetrics(SM_MOUSEPRESENT)) {
      BOOST_LOG(info) << "A mouse was not detected. Sunshine will enable Mouse Keys while streaming to force the mouse cursor to appear.";

      // Get the current state of Mouse Keys so we can restore it when streaming is over
      previous_mouse_keys_state.cbSize = sizeof(previous_mouse_keys_state);
      if (SystemParametersInfoW(SPI_GETMOUSEKEYS, 0, &previous_mouse_keys_state, 0)) {
        MOUSEKEYS new_mouse_keys_state = {};

        // Enable Mouse Keys
        new_mouse_keys_state.cbSize = sizeof(new_mouse_keys_state);
        new_mouse_keys_state.dwFlags = MKF_MOUSEKEYSON | MKF_AVAILABLE;
        new_mouse_keys_state.iMaxSpeed = 10;
        new_mouse_keys_state.iTimeToMaxSpeed = 1000;
        if (SystemParametersInfoW(SPI_SETMOUSEKEYS, 0, &new_mouse_keys_state, 0)) {
          // Remember to restore the previous settings when we stop streaming
          enabled_mouse_keys = true;
        } else {
          auto winerr = GetLastError();
          BOOST_LOG(warning) << "Unable to enable Mouse Keys: "sv << winerr;
        }
      } else {
        auto winerr = GetLastError();
        BOOST_LOG(warning) << "Unable to get current state of Mouse Keys: "sv << winerr;
      }
    }
  }

  void streaming_will_stop() {
    // Demote ourselves back to normal priority class
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    // End our 0.5ms timer request
    if (used_nt_set_timer_resolution) {
      used_nt_set_timer_resolution = false;
      if (!nt_set_timer_resolution_min()) {
        BOOST_LOG(error) << "nt_set_timer_resolution_min() failed even though nt_set_timer_resolution_max() succeeded";
      }
    } else {
      timeEndPeriod(1);
    }

    // Disable MMCSS scheduling for DWM
    DwmEnableMMCSS(false);

    // Closing our WLAN client handle will undo our optimizations
    if (wlan_handle != nullptr) {
      fn_WlanCloseHandle(wlan_handle, nullptr);
      wlan_handle = nullptr;
    }

    // Restore Mouse Keys back to the previous settings if we turned it on
    if (enabled_mouse_keys) {
      enabled_mouse_keys = false;
      if (!SystemParametersInfoW(SPI_SETMOUSEKEYS, 0, &previous_mouse_keys_state, 0)) {
        auto winerr = GetLastError();
        BOOST_LOG(warning) << "Unable to restore original state of Mouse Keys: "sv << winerr;
      }
    }
  }

  void restart_on_exit() {
    STARTUPINFOEXW startup_info {};
    startup_info.StartupInfo.cb = sizeof(startup_info);

    std::array<WCHAR, MAX_PATH> executable;
    if (GetModuleFileNameW(nullptr, executable.data(), executable.size()) == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(fatal) << "Failed to get Sunshine path: "sv << winerr;
      return;
    }

    PROCESS_INFORMATION process_info;
    if (!CreateProcessW(executable.data(), GetCommandLineW(), nullptr, nullptr, false, CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, (LPSTARTUPINFOW) &startup_info, &process_info)) {
      auto winerr = GetLastError();
      BOOST_LOG(fatal) << "Unable to restart Sunshine: "sv << winerr;
      return;
    }

    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
  }

  void restart() {
    // If we're running standalone, we have to respawn ourselves via CreateProcess().
    // If we're running from the service, we should just exit and let it respawn us.
    if (GetConsoleWindow() != nullptr) {
      // Avoid racing with the new process by waiting until we're exiting to start it.
      atexit(restart_on_exit);
    }

    // We use an async exit call here because we can't block the HTTP thread or we'll hang shutdown.
    lifetime::exit_sunshine(0, true);
  }

  /**
   * @brief Stores state while enumerating top-level Windows windows.
   */
  struct enum_wnd_context_t {
    std::set<DWORD> process_ids;  ///< Process ids.
    bool requested_exit;  ///< Whether a close request was observed while enumerating windows.
  };

  static BOOL CALLBACK prgrp_enum_windows(HWND hwnd, LPARAM lParam) {
    auto enum_ctx = (enum_wnd_context_t *) lParam;

    // Find the owner PID of this window
    DWORD wnd_process_id;
    if (!GetWindowThreadProcessId(hwnd, &wnd_process_id)) {
      // Continue enumeration
      return TRUE;
    }

    // Check if this window is owned by a process we want to terminate
    if (enum_ctx->process_ids.find(wnd_process_id) != enum_ctx->process_ids.end()) {
      // Send an async WM_CLOSE message to this window
      if (SendNotifyMessageW(hwnd, WM_CLOSE, 0, 0)) {
        BOOST_LOG(debug) << "Sent WM_CLOSE to PID: "sv << wnd_process_id;
        enum_ctx->requested_exit = true;
      } else {
        auto close_error = GetLastError();
        BOOST_LOG(warning) << "Failed to send WM_CLOSE to PID ["sv << wnd_process_id << "]: " << close_error;
      }
    }

    // Continue enumeration
    return TRUE;
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    auto job_handle = (HANDLE) native_handle;

    // Get list of all processes in our job object
    bool success;
    DWORD required_length = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST);
    auto process_id_list = (PJOBOBJECT_BASIC_PROCESS_ID_LIST) calloc(1, required_length);
    auto fg = util::fail_guard([&process_id_list]() {
      free(process_id_list);
    });
    while (!(success = QueryInformationJobObject(job_handle, JobObjectBasicProcessIdList, process_id_list, required_length, &required_length)) &&
           GetLastError() == ERROR_MORE_DATA) {
      free(process_id_list);
      process_id_list = (PJOBOBJECT_BASIC_PROCESS_ID_LIST) calloc(1, required_length);
      if (!process_id_list) {
        return false;
      }
    }

    if (!success) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "Failed to enumerate processes in group: "sv << err;
      return false;
    } else if (process_id_list->NumberOfProcessIdsInList == 0) {
      // If all processes are already dead, treat it as a success
      return true;
    }

    enum_wnd_context_t enum_ctx = {};
    enum_ctx.requested_exit = false;
    for (DWORD i = 0; i < process_id_list->NumberOfProcessIdsInList; i++) {
      enum_ctx.process_ids.emplace(process_id_list->ProcessIdList[i]);
    }

    // Enumerate all windows belonging to processes in the list
    EnumWindows(prgrp_enum_windows, (LPARAM) &enum_ctx);

    // Return success if we told at least one window to close
    return enum_ctx.requested_exit;
  }

  bool process_group_running(std::uintptr_t native_handle) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting_info;

    if (!QueryInformationJobObject((HANDLE) native_handle, JobObjectBasicAccountingInformation, &accounting_info, sizeof(accounting_info), nullptr)) {
      auto err = GetLastError();
      BOOST_LOG(error) << "Failed to get job accounting info: "sv << err;
      return false;
    }

    return accounting_info.ActiveProcesses != 0;
  }

  SOCKADDR_IN to_sockaddr(boost::asio::ip::address_v4 address, uint16_t port) {
    SOCKADDR_IN saddr_v4 = {};

    saddr_v4.sin_family = AF_INET;
    saddr_v4.sin_port = htons(port);

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v4.sin_addr, addr_bytes.data(), sizeof(saddr_v4.sin_addr));

    return saddr_v4;
  }

  SOCKADDR_IN6 to_sockaddr(boost::asio::ip::address_v6 address, uint16_t port) {
    SOCKADDR_IN6 saddr_v6 = {};

    saddr_v6.sin6_family = AF_INET6;
    saddr_v6.sin6_port = htons(port);
    saddr_v6.sin6_scope_id = address.scope_id();

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v6.sin6_addr, addr_bytes.data(), sizeof(saddr_v6.sin6_addr));

    return saddr_v6;
  }

  /**
   * @brief Write the source IP address into the first ancillary control message.
   *
   * @param msg Socket message with control storage sized for packet information.
   * @param source_address Local IP address to use for outgoing packets.
   * @return Bytes occupied by the packet-information control message.
   */
  static ULONG populate_source_pktinfo(WSAMSG &msg, const boost::asio::ip::address &source_address) {
    ULONG cmbuflen = 0;
    auto cm = WSA_CMSG_FIRSTHDR(&msg);
    if (source_address.is_v6()) {
      IN6_PKTINFO pktInfo;

      SOCKADDR_IN6 saddr_v6 = to_sockaddr(source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));

      cm->cmsg_level = IPPROTO_IPV6;
      cm->cmsg_type = IPV6_PKTINFO;
      cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
      memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
    } else {
      IN_PKTINFO pktInfo;

      SOCKADDR_IN saddr_v4 = to_sockaddr(source_address.to_v4(), 0);
      pktInfo.ipi_addr = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));

      cm->cmsg_level = IPPROTO_IP;
      cm->cmsg_type = IP_PKTINFO;
      cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
      memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
    }

    return cmbuflen;
  }

  // Use UDP segmentation offload if it is supported by the OS. If the NIC is capable, this will use
  // hardware acceleration to reduce CPU usage. Support for USO was introduced in Windows 10 20H1.
  bool send_batch(const batched_send_info_t &send_info) {
    WSAMSG msg;

    // Convert the target address into a SOCKADDR
    SOCKADDR_IN taddr_v4;
    SOCKADDR_IN6 taddr_v6;
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v6;
      msg.namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v4;
      msg.namelen = sizeof(taddr_v4);
    }

    auto const max_bufs_per_msg = send_info.payload_buffers.size() + (send_info.headers ? 1 : 0);

    std::vector<WSABUF> bufs((send_info.headers ? send_info.block_count : 1) * max_bufs_per_msg);
    DWORD bufcount = 0;
    if (send_info.headers) {
      // Interleave buffers for headers and payloads
      for (auto i = 0; i < send_info.block_count; i++) {
        bufs[bufcount].buf = (char *) &send_info.headers[(send_info.block_offset + i) * send_info.header_size];
        bufs[bufcount].len = send_info.header_size;
        bufcount++;
        auto payload_desc = send_info.buffer_for_payload_offset((send_info.block_offset + i) * send_info.payload_size);
        bufs[bufcount].buf = (char *) payload_desc.buffer;
        bufs[bufcount].len = send_info.payload_size;
        bufcount++;
      }
    } else {
      // Translate buffer descriptors into WSABUFs
      auto payload_offset = send_info.block_offset * send_info.payload_size;
      auto payload_length = payload_offset + (send_info.block_count * send_info.payload_size);
      while (payload_offset < payload_length) {
        auto payload_desc = send_info.buffer_for_payload_offset(payload_offset);
        bufs[bufcount].buf = (char *) payload_desc.buffer;
        bufs[bufcount].len = std::min(payload_desc.size, payload_length - payload_offset);
        payload_offset += bufs[bufcount].len;
        bufcount++;
      }
    }

    msg.lpBuffers = bufs.data();
    msg.dwBufferCount = bufcount;
    msg.dwFlags = 0;

    // At most, one DWORD option and one PKTINFO option
    std::array<char, WSA_CMSG_SPACE(sizeof(DWORD)) + std::max(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)), WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))> cmbuf = {};
    msg.Control.buf = cmbuf.data();
    msg.Control.len = cmbuf.size();

    auto cmbuflen = populate_source_pktinfo(msg, send_info.source_address);

    if (send_info.block_count > 1) {
      cmbuflen += WSA_CMSG_SPACE(sizeof(DWORD));

      auto cm = WSA_CMSG_NXTHDR(&msg, WSA_CMSG_FIRSTHDR(&msg));
      cm->cmsg_level = IPPROTO_UDP;
      cm->cmsg_type = UDP_SEND_MSG_SIZE;
      cm->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
      *((DWORD *) WSA_CMSG_DATA(cm)) = send_info.header_size + send_info.payload_size;
    }

    msg.Control.len = cmbuflen;

    // If USO is not supported, this will fail and the caller will fall back to unbatched sends.
    DWORD bytes_sent;
    return WSASendMsg((SOCKET) send_info.native_socket, &msg, 0, &bytes_sent, nullptr, nullptr) != SOCKET_ERROR;
  }

  bool send(send_info_t &send_info) {
    WSAMSG msg;

    // Convert the target address into a SOCKADDR
    SOCKADDR_IN taddr_v4;
    SOCKADDR_IN6 taddr_v6;
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v6;
      msg.namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v4;
      msg.namelen = sizeof(taddr_v4);
    }

    std::array<WSABUF, 2> bufs;
    DWORD bufcount = 0;
    if (send_info.header) {
      bufs[bufcount].buf = (char *) send_info.header;
      bufs[bufcount].len = send_info.header_size;
      bufcount++;
    }
    bufs[bufcount].buf = (char *) send_info.payload;
    bufs[bufcount].len = send_info.payload_size;
    bufcount++;

    msg.lpBuffers = bufs.data();
    msg.dwBufferCount = bufcount;
    msg.dwFlags = 0;

    std::array<char, std::max(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)), WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))> cmbuf = {};
    msg.Control.buf = cmbuf.data();
    msg.Control.len = cmbuf.size();

    auto cmbuflen = populate_source_pktinfo(msg, send_info.source_address);

    msg.Control.len = cmbuflen;

    DWORD bytes_sent;
    if (WSASendMsg((SOCKET) send_info.native_socket, &msg, 0, &bytes_sent, nullptr, nullptr) == SOCKET_ERROR) {
      auto winerr = WSAGetLastError();
      BOOST_LOG(warning) << "WSASendMsg() failed: "sv << winerr;
      return false;
    }

    return true;
  }

  /**
   * @brief Owns platform QoS state that is restored during cleanup.
   */
  class qos_t: public deinit_t {
  public:
    /**
     * @brief Explicitly store a Windows QoS flow ID for cleanup on destruction.
     *
     * @param flow_id Flow ID.
     */
    explicit qos_t(QOS_FLOWID flow_id):
        flow_id(flow_id) {
    }

    virtual ~qos_t() {
      if (!fn_QOSRemoveSocketFromFlow(qos_handle, (SOCKET) nullptr, flow_id, 0)) {
        auto winerr = GetLastError();
        BOOST_LOG(warning) << "QOSRemoveSocketFromFlow() failed: "sv << winerr;
      }
    }

  private:
    QOS_FLOWID flow_id;
  };

  /**
   * @brief Enables QoS on the given socket for traffic to the specified destination.
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging) {
    SOCKADDR_IN saddr_v4;
    SOCKADDR_IN6 saddr_v6;
    PSOCKADDR dest_addr;
    bool using_connect_hack = false;

    // Windows doesn't support any concept of traffic priority without DSCP tagging
    if (!dscp_tagging) {
      return nullptr;
    }

    static std::once_flag load_qwave_once_flag;
    std::call_once(load_qwave_once_flag, []() {
      // qWAVE is not installed by default on Windows Server, so we load it dynamically
      HMODULE qwave = LoadLibraryExA("qwave.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!qwave) {
        BOOST_LOG(debug) << "qwave.dll is not available on this OS"sv;
        return;
      }

      fn_QOSCreateHandle = (decltype(fn_QOSCreateHandle)) GetProcAddress(qwave, "QOSCreateHandle");
      fn_QOSAddSocketToFlow = (decltype(fn_QOSAddSocketToFlow)) GetProcAddress(qwave, "QOSAddSocketToFlow");
      fn_QOSRemoveSocketFromFlow = (decltype(fn_QOSRemoveSocketFromFlow)) GetProcAddress(qwave, "QOSRemoveSocketFromFlow");

      if (!fn_QOSCreateHandle || !fn_QOSAddSocketToFlow || !fn_QOSRemoveSocketFromFlow) {
        BOOST_LOG(error) << "qwave.dll is missing exports?"sv;

        fn_QOSCreateHandle = nullptr;
        fn_QOSAddSocketToFlow = nullptr;
        fn_QOSRemoveSocketFromFlow = nullptr;

        FreeLibrary(qwave);
        return;
      }

      QOS_VERSION qos_version {1, 0};
      if (!fn_QOSCreateHandle(&qos_version, &qos_handle)) {
        auto winerr = GetLastError();
        BOOST_LOG(warning) << "QOSCreateHandle() failed: "sv << winerr;
        return;
      }
    });

    // If qWAVE is unavailable, just return
    if (!fn_QOSAddSocketToFlow || !qos_handle) {
      return nullptr;
    }

    auto disconnect_fg = util::fail_guard([&]() {
      if (using_connect_hack) {
        SOCKADDR_IN6 empty = {};
        empty.sin6_family = AF_INET6;
        if (connect((SOCKET) native_socket, (PSOCKADDR) &empty, sizeof(empty)) < 0) {
          auto wsaerr = WSAGetLastError();
          BOOST_LOG(error) << "qWAVE dual-stack workaround failed: "sv << wsaerr;
        }
      }
    });

    if (address.is_v6()) {
      auto address_v6 = address.to_v6();

      saddr_v6 = to_sockaddr(address_v6, port);
      dest_addr = (PSOCKADDR) &saddr_v6;

      // qWAVE doesn't properly support IPv4-mapped IPv6 addresses, nor does it
      // correctly support IPv4 addresses on a dual-stack socket (despite MSDN's
      // claims to the contrary). To get proper QoS tagging when hosting in dual
      // stack mode, we will temporarily connect() the socket to allow qWAVE to
      // successfully initialize a flow, then disconnect it again so WSASendMsg()
      // works later on.
      if (address_v6.is_v4_mapped()) {
        if (connect((SOCKET) native_socket, (PSOCKADDR) &saddr_v6, sizeof(saddr_v6)) < 0) {
          auto wsaerr = WSAGetLastError();
          BOOST_LOG(error) << "qWAVE dual-stack workaround failed: "sv << wsaerr;
        } else {
          BOOST_LOG(debug) << "Using qWAVE connect() workaround for QoS tagging"sv;
          using_connect_hack = true;
          dest_addr = nullptr;
        }
      }
    } else {
      saddr_v4 = to_sockaddr(address.to_v4(), port);
      dest_addr = (PSOCKADDR) &saddr_v4;
    }

    QOS_TRAFFIC_TYPE traffic_type;
    switch (data_type) {
      case qos_data_type_e::audio:
        traffic_type = QOSTrafficTypeVoice;
        break;
      case qos_data_type_e::video:
        traffic_type = QOSTrafficTypeAudioVideo;
        break;
      default:
        BOOST_LOG(error) << "Unknown traffic type: "sv << std::to_underlying(data_type);
        return nullptr;
    }

    QOS_FLOWID flow_id = 0;
    if (!fn_QOSAddSocketToFlow(qos_handle, (SOCKET) native_socket, dest_addr, traffic_type, QOS_NON_ADAPTIVE_FLOW, &flow_id)) {
      auto winerr = GetLastError();
      BOOST_LOG(warning) << "QOSAddSocketToFlow() failed: "sv << winerr;
      return nullptr;
    }

    return std::make_unique<qos_t>(flow_id);
  }

  /**
   * @brief Read the current Windows high-resolution performance counter.
   */
  int64_t qpc_counter() {
    if (LARGE_INTEGER performance_counter; QueryPerformanceCounter(&performance_counter)) {
      return performance_counter.QuadPart;
    }
    return 0;
  }

  /**
   * @brief Convert the difference between two QPC readings to nanoseconds.
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2) {
    auto get_frequency = []() {
      LARGE_INTEGER frequency;
      frequency.QuadPart = 0;
      QueryPerformanceFrequency(&frequency);
      return frequency.QuadPart;
    };
    if (static const double frequency = get_frequency(); frequency) {
      return std::chrono::nanoseconds((int64_t) ((performance_counter1 - performance_counter2) * frequency / std::nano::den));
    }
    return {};
  }

  std::string get_host_name() {
    std::array<WCHAR, 256> hostname;
    if (GetHostNameW(hostname.data(), hostname.size()) == SOCKET_ERROR) {
      BOOST_LOG(error) << "GetHostNameW() failed: "sv << WSAGetLastError();
      return "Sunshine"s;
    }
    return utf_utils::to_utf8(hostname.data());
  }

  /**
   * @brief Implements the high-precision timer using Win32 waitable timers.
   */
  class win32_high_precision_timer: public high_precision_timer {
  public:
    win32_high_precision_timer() {
      // Use CREATE_WAITABLE_TIMER_HIGH_RESOLUTION if supported (Windows 10 1809+)
      timer = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (!timer) {
        timer = CreateWaitableTimerEx(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!timer) {
          BOOST_LOG(error) << "Unable to create high_precision_timer, CreateWaitableTimerEx() failed: " << GetLastError();
        }
      }
    }

    ~win32_high_precision_timer() {
      if (timer) {
        CloseHandle(timer);
      }
    }

    void sleep_for(const std::chrono::nanoseconds &duration) override {
      if (!timer) {
        BOOST_LOG(error) << "Attempting high_precision_timer::sleep_for() with uninitialized timer";
        return;
      }
      if (duration < 0s) {
        BOOST_LOG(error) << "Attempting high_precision_timer::sleep_for() with negative duration";
        return;
      }
      if (duration > 5s) {
        BOOST_LOG(error) << "Attempting high_precision_timer::sleep_for() with unexpectedly large duration (>5s)";
        return;
      }

      LARGE_INTEGER due_time;
      due_time.QuadPart = duration.count() / -100;
      SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, false);
      WaitForSingleObject(timer, INFINITE);
    }

    /**
     * @brief Explicitly check whether the timer backend is initialized.
     *
     * @return True when the platform timer is available.
     */
    explicit operator bool() override {
      return timer != nullptr;
    }

  private:
    HANDLE timer = nullptr;
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<win32_high_precision_timer>();
  }

  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(file_path.wstring().c_str(), &handle);
    if (size == 0) {
      return false;
    }

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(file_path.wstring().c_str(), handle, size, buffer.data())) {
      return false;
    }

    VS_FIXEDFILEINFO *file_info = nullptr;
    if (UINT file_info_size = 0; !VerQueryValueW(buffer.data(), L"\\", (LPVOID *) &file_info, &file_info_size)) {
      return false;
    }

    DWORD major = HIWORD(file_info->dwFileVersionMS);
    DWORD minor = LOWORD(file_info->dwFileVersionMS);
    DWORD build = HIWORD(file_info->dwFileVersionLS);
    DWORD revision = LOWORD(file_info->dwFileVersionLS);

    version_str = std::format("{}.{}.{}.{}", major, minor, build, revision);

    return true;
  }

  std::string resolve_render_device() {
    return {};
  }
}  // namespace platf
