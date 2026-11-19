/**
 * @file src/platform/linux/misc.cpp
 * @brief Miscellaneous definitions for Linux.
 */

// Required for in6_pktinfo with glibc headers
#ifndef _GNU_SOURCE
  /**
   * @def _GNU_SOURCE
   * @brief Macro for GNU SOURCE.
   */
  #define _GNU_SOURCE 1
#endif

// standard includes
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

// platform includes
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <gio/gio.h>  // For RTKit
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <pwd.h>
#include <sys/resource.h>  // For setpriority
#include <sys/socket.h>
#include <unistd.h>

#if !defined(__FreeBSD__)
  #include <sys/capability.h>
  #include <sys/prctl.h>
#endif
#ifdef __FreeBSD__
  #include <net/if_dl.h>  // For sockaddr_dl, LLADDR, and AF_LINK
  #include <sys/syscall.h>  // For syscall: SYS_thr_self
  #include <sys/thr.h>  // For thr_self
#endif

// lib includes
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <lizardbyte/common/env.h>

#ifdef SUNSHINE_BUILD_DRM
  #include <dirent.h>
  #include <xf86drm.h>
  #include <xf86drmMode.h>
#endif

// local includes
#include "graphics.h"
#include "misc.h"
#include "src/compat/boost_process.h"
#include "src/config.h"
#include "src/entry_handler.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/permissions.h"
#include "vaapi.h"

#ifdef __GNUC__
  #define SUNSHINE_GNUC_EXTENSION __extension__
#else
  /**
   * @def SUNSHINE_GNUC_EXTENSION
   * @brief Macro for SUNSHINE GNUC EXTENSION.
   */
  #define SUNSHINE_GNUC_EXTENSION
#endif

#ifndef SOL_IP
  /**
   * @def SOL_IP
   * @brief Macro for SOL IP.
   */
  #define SOL_IP IPPROTO_IP
#endif
#ifndef SOL_IPV6
  /**
   * @def SOL_IPV6
   * @brief Macro for SOL IPv6.
   */
  #define SOL_IPV6 IPPROTO_IPV6
#endif
#ifndef SOL_UDP
  /**
   * @def SOL_UDP
   * @brief Macro for SOL UDP.
   */
  #define SOL_UDP IPPROTO_UDP
#endif

using namespace std::literals;
namespace fs = std::filesystem;
namespace bp = boost::process::v1;

window_system_e window_system;  ///< Window system.

namespace dyn {
  /**
   * @brief Open the first available shared library from a nonempty list.
   */
  library_t *handle(const std::vector<const char *> &libs) {
    void *handle;

    for (auto lib : libs) {
      handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL);
      if (handle) {
        return static_cast<library_t *>(handle);
      }
    }

    std::stringstream ss;
    ss << "Couldn't find any of the following libraries: ["sv << libs.front();
    std::for_each(std::begin(libs) + 1, std::end(libs), [&](auto lib) {
      ss << ", "sv << lib;
    });

    ss << ']';

    BOOST_LOG(error) << ss.str();

    return nullptr;
  }

  /**
   * @brief Load named functions from an open shared library.
   */
  int load(library_t *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict) {
    int err = 0;
    for (auto &func : funcs) {
      TUPLE_2D_REF(fn, name, func);

      *fn = SUNSHINE_GNUC_EXTENSION(apiproc) dlsym(handle, name);

      if (!*fn && strict) {
        BOOST_LOG(error) << "Couldn't find function: "sv << name;

        err = -1;
      }
    }

    return err;
  }
}  // namespace dyn

namespace platf {
  std::vector<permission_status_t> get_permission_statuses() {
    // Match libvirtualhid's device paths and its read/write access check.
    bool input_access = access("/dev/uinput", R_OK | W_OK) == 0;
#ifndef __FreeBSD__
    input_access = input_access || access("/dev/input/uinput", R_OK | W_OK) == 0;
#endif
    return {{"input", input_access ? "granted" : "denied", config::input.keyboard || config::input.mouse || config::input.controller, true}};
  }

  bool request_permission(std::string_view id) {
    // Unix device access has no process-local permission prompt. The Web UI
    // presents the group/device setup steps for this known permission.
    (void) id;
    return false;
  }

  namespace {
    constexpr std::array privileged_gui_environment_variables {
      "GDK_PIXBUF_MODULEDIR",
      "GDK_PIXBUF_MODULE_FILE",
      "GIO_EXTRA_MODULES",
      "GTK3_MODULES",
      "GTK_EXE_PREFIX",
      "GTK_IM_MODULE_FILE",
      "GTK_MODULES",
      "GTK_PATH",
      "QML2_IMPORT_PATH",
      "QML_IMPORT_PATH",
      "QT_PLUGIN_PATH",
      "QT_QPA_PLATFORM_PLUGIN_PATH",
    };

  }  // namespace

  bool sanitize_process_environment() {
    for (const auto *variable : privileged_gui_environment_variables) {
      if (unsetenv(variable) != 0) {
        return false;
      }
    }
    return true;
  }

  bool log_missing_graphical_session_hint() {
    // KMS capture runs without a session, so the hint does not apply to it.
    if (std::string value; config::video.capture == "kms" || lizardbyte::common::get_env("WAYLAND_DISPLAY", value) || lizardbyte::common::get_env("DISPLAY", value)) {
      return false;
    }

    // Without a session the encoder probe fails too, and its final message
    // points at the display instead of at the missing session.
    BOOST_LOG(error) << "Neither WAYLAND_DISPLAY nor DISPLAY is set, so Sunshine is not running inside a graphical session. "sv
                     << "If it runs as a systemd user service, make sure the session exports its environment to systemd, "sv
                     << "for example with \"systemctl --user import-environment WAYLAND_DISPLAY DISPLAY\"."sv;
    return true;
  }

  /**
   * @brief Owning pointer for `getifaddrs` results.
   */
  using ifaddr_t = util::safe_ptr<ifaddrs, &freeifaddrs>;

  /**
   * @brief Open a DRM card node, dropping implicit DRM master when possible.
   *
   * See `misc.h` for full documentation. Master check/drop failures are logged
   * as warnings but do not fail the call.
   */
  int open_drm_card_fd(const std::filesystem::path &path, int flags) {
#ifdef SUNSHINE_BUILD_DRM
    int fd = open(path.c_str(), flags | O_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(error) << "Couldn't open: "sv << path.string() << ": "sv << strerror(errno);
      return -1;
    }

    auto is_master = [&]() -> int {
      drm_auth_t auth {};
      auth.magic = 0;

      errno = 0;
      if (drmIoctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) == 0) {
        return 1;  ///< AUTH_MAGIC succeeded, so we are master.
      }

      auto err = errno;
      if (err == EACCES) {
        return 0;  ///< Kernel rejected the ioctl because we are not master.
      }

      if (err == EINVAL || err == ENOENT) {
        return 1;  ///< Ioctl reached the master path but the magic (0) was invalid; we are master.
      }

      BOOST_LOG(warning) << "Couldn't determine DRM master state for "sv << path.string() << ": "sv << strerror(err);
      return -1;
    };

    auto master = is_master();
    if (master < 0) {
      BOOST_LOG(warning) << "Proceeding without dropping DRM master for "sv << path.string()
                         << "; compositor VT switches may fail."sv;
      return fd;
    }

    if (master) {
      if (drmDropMaster(fd)) {
        auto err = errno;
        BOOST_LOG(warning) << "Couldn't drop DRM master for "sv << path.string() << ": "sv << strerror(err)
                           << ", Compositor VT switches may fail."sv;
        return fd;
      }

      BOOST_LOG(info) << "Dropped DRM master for "sv << path.string();

      master = is_master();
      if (master < 0) {
        BOOST_LOG(warning) << "Could not re-verify DRM master state after drop for "sv << path.string() << "."sv;
        return fd;
      }

      if (master) {
        BOOST_LOG(warning) << "Still DRM master after drop for "sv << path.string() << "."sv;
        return fd;
      }
    }

    return fd;
#else
  #ifndef __FreeBSD__
    BOOST_LOG(info) << "Sunshine compiled without DRM support. Cannot control Linux DRM master state for "sv << path.string();
  #endif
    return open(path.c_str(), flags | O_CLOEXEC);
#endif
  }

  /**
   * @brief Read the local interface address list.
   *
   * @return Owning pointer to the interface address list, or nullptr on failure.
   */
  ifaddr_t get_ifaddrs() {
    ifaddrs *p {nullptr};

    getifaddrs(&p);

    return ifaddr_t {p};
  }

  /**
   * @brief Performs migration if necessary, then returns the appdata directory.
   * @details This is used for the log directory, so it cannot invoke Boost logging!
   * @return The path of the appdata directory that should be used.
   */
  fs::path appdata() {
    static std::once_flag migration_flag;
    static fs::path config_path;

    // Ensure migration is only attempted once
    std::call_once(migration_flag, []() {
      bool found = false;
      bool migrate_config = true;
      fs::path homedir {lizardbyte::common::get_env("HOME")};

      // Get the home directory
      if (homedir.empty()) {
        // If HOME is empty or not set, use the current user's home directory
        homedir = getpwuid(geteuid())->pw_dir;
      }

      // May be set if running under a systemd service with the ConfigurationDirectory= option set.
      if (std::string dir; lizardbyte::common::get_env("CONFIGURATION_DIRECTORY", dir) && !dir.empty()) {
        found = true;
        config_path = fs::path(dir) / "sunshine"sv;
      }
      // Otherwise, follow the XDG base directory specification:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      if (std::string dir; !found && lizardbyte::common::get_env("XDG_CONFIG_HOME", dir) && !dir.empty()) {
        found = true;
        config_path = fs::path(dir) / "sunshine"sv;
      }
      // As a last resort, use the home directory
      if (!found) {
        migrate_config = false;
        config_path = homedir / ".config" / "sunshine";
      }

      // migrate from the old config location if necessary
      if (std::string migrate_envvar; migrate_config && found && lizardbyte::common::get_env("SUNSHINE_MIGRATE_CONFIG", migrate_envvar) && migrate_envvar == "1") {
        std::error_code ec;
        fs::path old_config_path = homedir / ".config" / "sunshine";
        if (old_config_path != config_path && fs::exists(old_config_path, ec)) {
          if (!fs::exists(config_path, ec)) {
            std::cout << "Migrating config from "sv << old_config_path << " to "sv << config_path << std::endl;
            if (!ec) {
              // Create the new directory tree if it doesn't already exist
              fs::create_directories(config_path, ec);
            }
            if (!ec) {
              // Copy the old directory into the new location
              // NB: We use a copy instead of a move so that cross-volume migrations work
              fs::copy(old_config_path, config_path, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            }
            if (!ec) {
              // If the copy was successful, delete the original directory
              fs::remove_all(old_config_path, ec);
              if (ec) {
                std::cerr << "Failed to clean up old config directory: " << ec.message() << std::endl;

                // This is not fatal. Next time we start, we'll warn the user to delete the old one.
                ec.clear();
              }
            }
            if (ec) {
              std::cerr << "Migration failed: " << ec.message() << std::endl;
              config_path = old_config_path;
            }
          } else {
            // We cannot use Boost logging because it hasn't been initialized yet!
            std::cerr << "Config exists in both "sv << old_config_path << " and "sv << config_path << ". Using "sv << config_path << " for config" << std::endl;
            std::cerr << "It is recommended to remove "sv << old_config_path << std::endl;
          }
        }
      }
    });

    return config_path;
  }

  /**
   * @brief Get path of the XDG Portal restore token.
   *
   * @return Path of token that may include the session desktop as a suffix (e.g. .gnome, .kde).
   */
  std::filesystem::path get_xdg_restore_token_path() {
#ifdef SUNSHINE_BUILD_PORTAL
    return portal::get_saved_token_path();
#else
    return appdata() / "portal_token";
#endif
  }

  /**
   * @brief Convert a socket address to a printable IP address.
   */
  std::string from_sockaddr(const sockaddr *const ip_addr) {
    std::array<char, INET6_ADDRSTRLEN> data = {};

    auto family = ip_addr->sa_family;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data.data(), INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data.data(), INET_ADDRSTRLEN);
    }

    return std::string {data.data()};
  }

  /**
   * @brief Convert a socket address to a port and printable IP address.
   */
  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    std::array<char, INET6_ADDRSTRLEN> data = {};

    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data.data(), INET6_ADDRSTRLEN);
      port = ((sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data.data(), INET_ADDRSTRLEN);
      port = ((sockaddr_in *) ip_addr)->sin_port;
    }

    return {port, std::string {data.data()}};
  }

  /**
   * @brief Return the hardware MAC address associated with a network address.
   */
  std::string get_mac_address(const std::string_view &address) {
    auto ifaddrs = get_ifaddrs();

#ifdef __FreeBSD__
    // On FreeBSD, we need to find the interface name first, then look for its AF_LINK entry
    std::string interface_name;
    for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
      if (pos->ifa_addr && address == from_sockaddr(pos->ifa_addr)) {
        interface_name = pos->ifa_name;
        break;
      }
    }

    if (!interface_name.empty()) {
      // Find the AF_LINK entry for this interface to get MAC address
      for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
        if (pos->ifa_addr && pos->ifa_addr->sa_family == AF_LINK && interface_name == pos->ifa_name) {
          auto sdl = (struct sockaddr_dl *) pos->ifa_addr;
          auto mac = (unsigned char *) LLADDR(sdl);

          // Format MAC address as XX:XX:XX:XX:XX:XX
          std::ostringstream mac_stream;
          mac_stream << std::hex << std::setfill('0');
          for (int i = 0; i < sdl->sdl_alen; i++) {
            if (i > 0) {
              mac_stream << ':';
            }
            mac_stream << std::setw(2) << (int) mac[i];
          }
          return mac_stream.str();
        }
      }
    }
#else
    // On Linux, read MAC address from sysfs
    for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
      if (pos->ifa_addr && address == from_sockaddr(pos->ifa_addr)) {
        std::ifstream mac_file("/sys/class/net/"s + pos->ifa_name + "/address");
        if (mac_file.good()) {
          std::string mac_address;
          std::getline(mac_file, mac_address);
          return mac_address;
        }
      }
    }
#endif

    BOOST_LOG(warning) << "Unable to find MAC address for "sv << address;
    return "00:00:00:00:00:00"s;
  }

  /**
   * @brief Launch a command using the caller's Linux privileges and session.
   * @details The common platform signature's elevation and interactivity flags do not apply on Linux.
   */
  bp::child run_command(bool, bool, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    // clang-format off
    if (!group) {
      if (!file) {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, bp::limit_handles, ec);
      }
      else {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, bp::limit_handles, ec);
      }
    }
    else {
      if (!file) {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > bp::null, bp::std_err > bp::null, bp::limit_handles, ec, *group);
      }
      else {
        return bp::child(cmd, env, bp::start_dir(working_dir), bp::std_in < bp::null, bp::std_out > file, bp::std_err > file, bp::limit_handles, ec, *group);
      }
    }
    // clang-format on
  }

  /**
   * @brief Open a url in the default web browser.
   * @param url The url to open.
   */
  void open_url(const std::string &url) {
    // set working dir to user home directory
    auto working_dir = boost::filesystem::path(lizardbyte::common::get_env("HOME"));
    std::string cmd = R"(xdg-open ")" + url + R"(")";

    boost::process::v1::environment _env = boost::this_process::environment();
    std::error_code ec;
    auto child = run_command(false, false, cmd, working_dir, _env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't open url ["sv << url << "]: System: "sv << ec.message();
    } else {
      BOOST_LOG(info) << "Opened url ["sv << url << "]"sv;
      child.detach();
    }
  }

  /**
   * @brief Apply the requested scheduling priority to the current thread.
   */
  void adjust_thread_priority(thread_priority_e priority) {
#if defined(__FreeBSD__)
    pid_t tid = syscall(SYS_thr_self);
#else
    pid_t tid = syscall(SYS_gettid);
#endif
    bool success = false;
    int32_t linux_nice;

    using enum thread_priority_e;
    switch (priority) {
      case low:
        linux_nice = 10;
        break;
      case normal:
        linux_nice = 0;
        break;
      case high:
        linux_nice = -10;
        break;
      case critical:
        linux_nice = -15;
        break;
      default:
        BOOST_LOG(debug) << "Unknown thread priority: "sv << std::to_underlying(priority);
        return;
    }

    g_autoptr(GError) err = nullptr;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);

    if (conn) {
      g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.RealtimeKit1",
        "/org/freedesktop/RealtimeKit1",
        "org.freedesktop.RealtimeKit1",
        "MakeThreadHighPriority",
        g_variant_new("(ti)", (guint64) tid, linux_nice),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &err
      );

      if (!err) {
        success = true;
        BOOST_LOG(debug) << "RTKit: Successfully set priority to "sv << linux_nice;
      } else {
        BOOST_LOG(debug) << "RTKit: Could not set priority: "sv << err->message;
        g_clear_error(&err);
      }
    }

    if (!success) {
      // This will run on FreeBSD OR Linux if RTKit failed/was missing
      if (setpriority(PRIO_PROCESS, 0, linux_nice) == -1) {
        BOOST_LOG(warning) << "setpriority failed for nice "sv << linux_nice << ": "sv << strerror(errno);
      } else {
        BOOST_LOG(debug) << "setpriority success for nice "sv << linux_nice;
      }
    }
  }

  void set_thread_name(std::string_view name) {
    // Truncate name to fit in Linux/FreeBSD kernel's 16 byte limit
    std::string tr_name {name.substr(0, 15)};
    pthread_setname_np(pthread_self(), tr_name.c_str());
  }

  /**
   * @brief Enable or disable X11 mouse keys for the current session.
   */
  void enable_mouse_keys() {
    // Unimplemented
  }

  /**
   * @brief Apply Linux platform state before streaming starts.
   */
  void streaming_will_start() {
    // Nothing to do
  }

  /**
   * @brief Restore Linux platform state after streaming stops.
   */
  void streaming_will_stop() {
    // Nothing to do
  }

  /**
   * @brief Request a Sunshine process restart on exit.
   */
  void restart_on_exit() {
    std::array<char, PATH_MAX> executable;
    ssize_t len = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (len == -1) {
      BOOST_LOG(fatal) << "readlink() failed: "sv << errno;
      return;
    }
    executable[len] = '\0';

    // ASIO doesn't use O_CLOEXEC, so we have to close all fds ourselves
    auto openmax = (int) sysconf(_SC_OPEN_MAX);
    for (int fd = STDERR_FILENO + 1; fd < openmax; fd++) {
      close(fd);
    }

    // Re-exec ourselves with the same arguments
    if (execv(executable.data(), lifetime::get_argv()) < 0) {
      BOOST_LOG(fatal) << "execv() failed: "sv << errno;
      return;
    }
  }

  /**
   * @brief Restart the Sunshine process through the platform launcher.
   */
  void restart() {
    // Gracefully clean up and restart ourselves instead of exiting
    atexit(restart_on_exit);
    lifetime::exit_sunshine(0, true);
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    if (kill(-((pid_t) native_handle), SIGTERM) == 0 || errno == ESRCH) {
      BOOST_LOG(debug) << "Successfully sent SIGTERM to process group: "sv << native_handle;
      return true;
    } else {
      BOOST_LOG(warning) << "Unable to send SIGTERM to process group ["sv << native_handle << "]: "sv << errno;
      return false;
    }
  }

  bool process_group_running(std::uintptr_t native_handle) {
    return waitpid(-((pid_t) native_handle), nullptr, WNOHANG) >= 0;
  }

  /**
   * @brief Convert to sockaddr.
   *
   * @param address Network address being parsed or filtered.
   * @param port TCP or UDP port number.
   * @return Value converted to sockaddr.
   */
  struct sockaddr_in to_sockaddr(boost::asio::ip::address_v4 address, uint16_t port) {
    struct sockaddr_in saddr_v4 = {};

    saddr_v4.sin_family = AF_INET;
    saddr_v4.sin_port = htons(port);

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v4.sin_addr, addr_bytes.data(), sizeof(saddr_v4.sin_addr));

    return saddr_v4;
  }

  /**
   * @brief Convert to sockaddr.
   *
   * @param address Network address being parsed or filtered.
   * @param port TCP or UDP port number.
   * @return Value converted to sockaddr.
   */
  struct sockaddr_in6 to_sockaddr(boost::asio::ip::address_v6 address, uint16_t port) {
    struct sockaddr_in6 saddr_v6 = {};

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
  static socklen_t populate_source_pktinfo(msghdr &msg, const boost::asio::ip::address &source_address) {
    socklen_t cmbuflen = 0;
    auto pktinfo_cm = CMSG_FIRSTHDR(&msg);
    if (source_address.is_v6()) {
      struct in6_pktinfo pktInfo;

      struct sockaddr_in6 saddr_v6 = to_sockaddr(source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IPV6;
      pktinfo_cm->cmsg_type = IPV6_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    } else {
#ifdef IP_PKTINFO
      struct in_pktinfo pktInfo;

      struct sockaddr_in saddr_v4 = to_sockaddr(source_address.to_v4(), 0);
      pktInfo.ipi_spec_dst = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IP;
      pktinfo_cm->cmsg_type = IP_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
#elif defined(IP_SENDSRCADDR)
      // FreeBSD uses IP_SENDSRCADDR with struct in_addr instead of IP_PKTINFO
      struct sockaddr_in saddr_v4 = to_sockaddr(source_address.to_v4(), 0);
      struct in_addr src_addr = saddr_v4.sin_addr;

      cmbuflen += CMSG_SPACE(sizeof(src_addr));

      pktinfo_cm->cmsg_level = IPPROTO_IP;
      pktinfo_cm->cmsg_type = IP_SENDSRCADDR;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(src_addr));
      memcpy(CMSG_DATA(pktinfo_cm), &src_addr, sizeof(src_addr));
#endif
    }

    return cmbuflen;
  }

  /**
   * @brief Send multiple fixed-size UDP payload blocks using the platform backend.
   */
  bool send_batch(const batched_send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    struct msghdr msg = {};

    // Convert the target address into a sockaddr
    struct sockaddr_in taddr_v4 = {};
    struct sockaddr_in6 taddr_v6 = {};
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v6;
      msg.msg_namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v4;
      msg.msg_namelen = sizeof(taddr_v4);
    }

    union {
#ifdef IP_PKTINFO
      std::array<char, CMSG_SPACE(sizeof(uint16_t)) + std::max(CMSG_SPACE(sizeof(struct in_pktinfo)), CMSG_SPACE(sizeof(struct in6_pktinfo)))> buf;
#elif defined(IP_SENDSRCADDR)
      // FreeBSD uses IP_SENDSRCADDR with struct in_addr instead of IP_PKTINFO with struct in_pktinfo
      std::array<char, CMSG_SPACE(sizeof(uint16_t)) + std::max(CMSG_SPACE(sizeof(struct in_addr)), CMSG_SPACE(sizeof(struct in6_pktinfo)))> buf;
#endif
      struct cmsghdr alignment;
    } cmbuf = {};  // Must be zeroed for CMSG_NXTHDR()

    msg.msg_control = cmbuf.buf.data();
    msg.msg_controllen = cmbuf.buf.size();

    // The PKTINFO option will always be first, then we will conditionally
    // append the UDP_SEGMENT option next if applicable.
    auto cmbuflen = populate_source_pktinfo(msg, send_info.source_address);

    auto const max_iovs_per_msg = send_info.payload_buffers.size() + (send_info.headers ? 1 : 0);

#ifdef UDP_SEGMENT
    {
      // UDP GSO on Linux currently only supports sending 64K or 64 segments at a time
      size_t seg_index = 0;
      const size_t seg_max = 65536 / 1500;
      std::vector<iovec> iovs((send_info.headers ? std::min(seg_max, send_info.block_count) : 1) * max_iovs_per_msg);
      auto msg_size = send_info.header_size + send_info.payload_size;
      while (seg_index < send_info.block_count) {
        int iovlen = 0;
        auto segs_in_batch = std::min(send_info.block_count - seg_index, seg_max);
        if (send_info.headers) {
          // Interleave iovs for headers and payloads
          for (auto i = 0; i < segs_in_batch; i++) {
            iovs[iovlen].iov_base = (void *) &send_info.headers[(send_info.block_offset + seg_index + i) * send_info.header_size];
            iovs[iovlen].iov_len = send_info.header_size;
            iovlen++;
            auto payload_desc = send_info.buffer_for_payload_offset((send_info.block_offset + seg_index + i) * send_info.payload_size);
            iovs[iovlen].iov_base = (void *) payload_desc.buffer;
            iovs[iovlen].iov_len = send_info.payload_size;
            iovlen++;
          }
        } else {
          // Translate buffer descriptors into iovs
          auto payload_offset = (send_info.block_offset + seg_index) * send_info.payload_size;
          auto payload_length = payload_offset + (segs_in_batch * send_info.payload_size);
          while (payload_offset < payload_length) {
            auto payload_desc = send_info.buffer_for_payload_offset(payload_offset);
            iovs[iovlen].iov_base = (void *) payload_desc.buffer;
            iovs[iovlen].iov_len = std::min(payload_desc.size, payload_length - payload_offset);
            payload_offset += iovs[iovlen].iov_len;
            iovlen++;
          }
        }

        msg.msg_iov = iovs.data();
        msg.msg_iovlen = iovlen;

        // We should not use GSO if the data is <= one full block size
        if (segs_in_batch > 1) {
          msg.msg_controllen = cmbuflen + CMSG_SPACE(sizeof(uint16_t));

          // Enable GSO to perform segmentation of our buffer for us
          auto cm = CMSG_NXTHDR(&msg, CMSG_FIRSTHDR(&msg));
          cm->cmsg_level = SOL_UDP;
          cm->cmsg_type = UDP_SEGMENT;
          cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
          *((uint16_t *) CMSG_DATA(cm)) = msg_size;
        } else {
          msg.msg_controllen = cmbuflen;
        }

        // This will fail if GSO is not available, so we will fall back to non-GSO if
        // it's the first sendmsg() call. On subsequent calls, we will treat errors as
        // actual failures and return to the caller.
        auto bytes_sent = sendmsg(sockfd, &msg, 0);
        if (bytes_sent < 0) {
          // If there's no send buffer space, wait for some to be available
          if (errno == EAGAIN) {
            struct pollfd pfd;

            pfd.fd = sockfd;
            pfd.events = POLLOUT;

            if (poll(&pfd, 1, -1) != 1) {
              BOOST_LOG(warning) << "poll() failed: "sv << errno;
              break;
            }

            // Try to send again
            continue;
          }

          BOOST_LOG(verbose) << "sendmsg() failed: "sv << errno;
          break;
        }

        seg_index += bytes_sent / msg_size;
      }

      // If we sent something, return the status and don't fall back to the non-GSO path.
      if (seg_index != 0) {
        return seg_index >= send_info.block_count;
      }
    }
#endif

    {
      // If GSO is not supported, use sendmmsg() instead.
      std::vector<struct mmsghdr> msgs(send_info.block_count);
      std::vector<struct iovec> iovs(send_info.block_count * (send_info.headers ? 2 : 1));
      int iov_idx = 0;
      for (size_t i = 0; i < send_info.block_count; i++) {
        msgs[i].msg_len = 0;
        msgs[i].msg_hdr.msg_iov = &iovs[iov_idx];
        msgs[i].msg_hdr.msg_iovlen = send_info.headers ? 2 : 1;

        if (send_info.headers) {
          iovs[iov_idx].iov_base = (void *) &send_info.headers[(send_info.block_offset + i) * send_info.header_size];
          iovs[iov_idx].iov_len = send_info.header_size;
          iov_idx++;
        }
        auto payload_desc = send_info.buffer_for_payload_offset((send_info.block_offset + i) * send_info.payload_size);
        iovs[iov_idx].iov_base = (void *) payload_desc.buffer;
        iovs[iov_idx].iov_len = send_info.payload_size;
        iov_idx++;

        msgs[i].msg_hdr.msg_name = msg.msg_name;
        msgs[i].msg_hdr.msg_namelen = msg.msg_namelen;
        msgs[i].msg_hdr.msg_control = cmbuf.buf.data();
        msgs[i].msg_hdr.msg_controllen = cmbuflen;
        msgs[i].msg_hdr.msg_flags = 0;
      }

      // Call sendmmsg() until all messages are sent
      size_t blocks_sent = 0;
      while (blocks_sent < send_info.block_count) {
        int msgs_sent = sendmmsg(sockfd, &msgs[blocks_sent], send_info.block_count - blocks_sent, 0);
        if (msgs_sent < 0) {
          // If there's no send buffer space, wait for some to be available
          if (errno == EAGAIN) {
            struct pollfd pfd;

            pfd.fd = sockfd;
            pfd.events = POLLOUT;

            if (poll(&pfd, 1, -1) != 1) {
              BOOST_LOG(warning) << "poll() failed: "sv << errno;
              break;
            }

            // Try to send again
            continue;
          }

          BOOST_LOG(warning) << "sendmmsg() failed: "sv << errno;
          return false;
        }

        blocks_sent += msgs_sent;
      }

      return true;
    }
  }

  /**
   * @brief Send the serialized response over the active socket.
   */
  bool send(send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    struct msghdr msg = {};

    // Convert the target address into a sockaddr
    struct sockaddr_in taddr_v4 = {};
    struct sockaddr_in6 taddr_v6 = {};
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v6;
      msg.msg_namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v4;
      msg.msg_namelen = sizeof(taddr_v4);
    }

    union {
#ifdef IP_PKTINFO
      std::array<char, std::max(CMSG_SPACE(sizeof(struct in_pktinfo)), CMSG_SPACE(sizeof(struct in6_pktinfo)))> buf;
#elif defined(IP_SENDSRCADDR)
      // FreeBSD uses IP_SENDSRCADDR with struct in_addr instead of IP_PKTINFO with struct in_pktinfo
      std::array<char, std::max(CMSG_SPACE(sizeof(struct in_addr)), CMSG_SPACE(sizeof(struct in6_pktinfo)))> buf;
#endif
      struct cmsghdr alignment;
    } cmbuf;

    msg.msg_control = cmbuf.buf.data();
    msg.msg_controllen = cmbuf.buf.size();

    auto cmbuflen = populate_source_pktinfo(msg, send_info.source_address);

    std::array<iovec, 2> iovs;
    int iovlen = 0;
    if (send_info.header) {
      iovs[iovlen].iov_base = (void *) send_info.header;
      iovs[iovlen].iov_len = send_info.header_size;
      iovlen++;
    }
    iovs[iovlen].iov_base = (void *) send_info.payload;
    iovs[iovlen].iov_len = send_info.payload_size;
    iovlen++;

    msg.msg_iov = iovs.data();
    msg.msg_iovlen = iovlen;

    msg.msg_controllen = cmbuflen;

    auto bytes_sent = sendmsg(sockfd, &msg, 0);

    // If there's no send buffer space, wait for some to be available
    while (bytes_sent < 0 && errno == EAGAIN) {
      struct pollfd pfd;

      pfd.fd = sockfd;
      pfd.events = POLLOUT;

      if (poll(&pfd, 1, -1) != 1) {
        BOOST_LOG(warning) << "poll() failed: "sv << errno;
        break;
      }

      // Try to send again
      bytes_sent = sendmsg(sockfd, &msg, 0);
    }

    if (bytes_sent < 0) {
      BOOST_LOG(warning) << "sendmsg() failed: "sv << errno;
      return false;
    }

    return true;
  }

  // We can't track QoS state separately for each destination on this OS,
  // so we keep a ref count to only disable QoS options when all clients
  // are disconnected.
  static std::atomic qos_ref_count = 0;

  /**
   * @brief Linux QoS state used to tune socket priority while streaming.
   */
  class qos_t: public deinit_t {
  public:
    /**
     * @brief Apply Linux socket priority and DSCP QoS settings for scoped cleanup.
     *
     * @param sockfd Native socket descriptor whose options are updated.
     * @param options Socket options copied into this cleanup object's owned storage.
     */
    qos_t(int sockfd, const std::vector<std::tuple<int, int, int>> &options):
        sockfd(sockfd),
        options(options) {
      qos_ref_count++;
    }

    virtual ~qos_t() {
      if (--qos_ref_count == 0) {
        for (const auto &[level, option, reset_val] : options) {
          if (setsockopt(sockfd, level, option, &reset_val, sizeof(reset_val)) < 0) {
            BOOST_LOG(warning) << "Failed to reset option: "sv << errno;
          }
        }
      }
    }

  private:
    int sockfd;
    std::vector<std::tuple<int, int, int>> options;
  };

  /**
   * @brief Enables QoS on the given socket for traffic to the specified destination.
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging) {
    auto sockfd = (int) native_socket;
    std::vector<std::tuple<int, int, int>> reset_options;

    if (dscp_tagging) {
      int level;
      int option;

      // With dual-stack sockets, Linux uses IPV6_TCLASS for IPv6 traffic
      // and IP_TOS for IPv4 traffic.
      if (address.is_v6() && !address.to_v6().is_v4_mapped()) {
        level = SOL_IPV6;
        option = IPV6_TCLASS;
      } else {
        level = SOL_IP;
        option = IP_TOS;
      }

      // The specific DSCP values here are chosen to be consistent with Windows,
      // except that we use CS6 instead of CS7 for audio traffic.
      int dscp = 0;
      switch (data_type) {
        case qos_data_type_e::video:
          dscp = 40;
          break;
        case qos_data_type_e::audio:
          dscp = 48;
          break;
        default:
          BOOST_LOG(error) << "Unknown traffic type: "sv << std::to_underlying(data_type);
          break;
      }

      if (dscp) {
        // Shift to put the DSCP value in the correct position in the TOS field
        dscp <<= 2;

        if (setsockopt(sockfd, level, option, &dscp, sizeof(dscp)) == 0) {
          // Reset TOS to -1 when QoS is disabled
          reset_options.emplace_back(std::make_tuple(level, option, -1));
        } else {
          BOOST_LOG(error) << "Failed to set TOS/TCLASS: "sv << errno;
        }
      }
    }

    // We can use SO_PRIORITY to set outgoing traffic priority without DSCP tagging.
    //
    // NB: We set this after IP_TOS/IPV6_TCLASS since setting TOS value seems to
    // reset SO_PRIORITY back to 0.
    //
    // 6 is the highest priority that can be used without SYS_CAP_ADMIN.
#ifndef SO_PRIORITY
    // FreeBSD doesn't support SO_PRIORITY, so we skip this
    BOOST_LOG(debug) << "SO_PRIORITY not supported on this platform, skipping traffic priority setting";
#else
    int priority = data_type == qos_data_type_e::audio ? 6 : 5;
    if (setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) == 0) {
      // Reset SO_PRIORITY to 0 when QoS is disabled
      reset_options.emplace_back(std::make_tuple(SOL_SOCKET, SO_PRIORITY, 0));
    } else {
      BOOST_LOG(error) << "Failed to set SO_PRIORITY: "sv << errno;
    }
#endif

    return std::make_unique<qos_t>(sockfd, reset_options);
  }

  std::string get_host_name() {
    try {
      return boost::asio::ip::host_name();
    } catch (boost::system::system_error &err) {
      BOOST_LOG(error) << "Failed to get hostname: "sv << err.what();
      return "Sunshine"s;
    }
  }

  namespace source {
    /**
     * @brief Enumerates supported source options.
     */
    enum source_e : std::size_t {
#ifdef SUNSHINE_BUILD_CUDA
      NVFBC,  ///< NvFBC
#endif
#ifdef SUNSHINE_BUILD_WAYLAND
      WAYLAND,  ///< Wayland
#endif
#ifdef SUNSHINE_BUILD_DRM
      KMS,  ///< KMS
#endif
#ifdef SUNSHINE_BUILD_X11
      X11,  ///< X11
#endif
#ifdef SUNSHINE_BUILD_KWIN
      KWIN,  ///< KWin ScreenCast
#endif
#ifdef SUNSHINE_BUILD_PORTAL
      PORTAL,  ///< XDG PORTAL
#endif
      MAX_FLAGS  ///< The maximum number of flags
    };
  }  // namespace source

  static std::bitset<source::MAX_FLAGS> sources;

#ifdef SUNSHINE_BUILD_CUDA
  std::vector<std::string> nvfbc_display_names();
  std::shared_ptr<display_t> nvfbc_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_nvfbc() {
    return !nvfbc_display_names().empty();
  }
#endif

#ifdef SUNSHINE_BUILD_WAYLAND
  /**
   * @brief Enumerate displays available through the Wayland capture backend.
   *
   * @return Wayland display names, or an empty list when discovery fails.
   */
  std::vector<std::string> wl_display_names();
  /**
   * @brief Create a Wayland display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend, or nullptr when Wayland capture initialization fails.
   */
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  /**
   * @brief Check whether Wayland capture is available for the current session.
   *
   * @return True when the active window system is Wayland and at least one output is discoverable.
   */
  bool verify_wl() {
    return window_system == window_system_e::WAYLAND && !wl_display_names().empty();
  }
#endif

#ifdef SUNSHINE_BUILD_DRM
  std::vector<std::string> kms_display_names(mem_type_e hwdevice_type);
  std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_kms() {
    return !kms_display_names(mem_type_e::unknown).empty();
  }
#endif

#ifdef SUNSHINE_BUILD_X11
  std::vector<std::string> x11_display_names();
  std::shared_ptr<display_t> x11_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_x11() {
    return window_system == window_system_e::X11 && !x11_display_names().empty();
  }
#endif

#ifdef SUNSHINE_BUILD_PORTAL
  std::vector<std::string> portal_display_names(bool allow_start_timeout);
  std::shared_ptr<display_t> portal_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  /**
   * @brief Enumerates possible Portal probe responses.
   */
  enum class portal_probe_e {
    unreachable,  ///< Portal service is unreachable.
    no_token,  ///< Token not found.
    stale_token,  ///< Had a token, but it didn't produce a working session.
    available  ///< Portal is available.
  };

  /**
   * @brief Probe Portal availability via token existence, DBus availability and timed probe.
   *
   * @return Can return no_token, unreachable, stale_token or available for processing via verify_portal().
   */
  portal_probe_e probe_portal() {
    using enum portal_probe_e;

    if (!portal::is_portal_service_reachable()) {
      return unreachable;
    }
    if (!portal::has_saved_token()) {
      return no_token;
    }
    return portal_display_names(true).empty() ? stale_token : available;
  }

  /**
   * @brief Verify Portal capture and/or begin token negotiation via TaskPool.
   *        If negotiation is requested, either queue a TaskPool task if no restore token is detected,
   *        or if a stale token is detected, delete it and restart Sunshine.
   *
   * @return True if Portal is available.
   */
  bool verify_portal() {
    using enum portal_probe_e;

    auto result = probe_portal();
    switch (result) {
      case available:
        return true;
      case unreachable:
        BOOST_LOG(debug) << "[portalgrab] xdg-desktop-portal not reachable; skipping Portal capture."sv;
        return false;
      case stale_token:
        BOOST_LOG(warning) << "[portalgrab] Saved portal token did not produce a session; discarding and restarting."sv;
        portal::clear_saved_token();
        platf::restart();
        return false;
      case no_token:
        BOOST_LOG(fatal) << "Portal capture is awaiting user permission. "sv
                         << "The current session will attempt to use a fallback capture method."sv;
        std::call_once(portal::xdg_worker_flag, []() {
          portal::xdg_worker = std::jthread([]() {
            try {
              platf::set_thread_name("xdg_worker");
              if (!portal_display_names(false).empty()) {
                platf::restart();
              } else {
                BOOST_LOG(error) << "[portalgrab] Portal session token was not negotiated."sv;
              }
            } catch (const std::exception &e) {
              BOOST_LOG(error) << "[portalgrab] Exception caught in xdg_worker: "sv << e.what();
            } catch (...) {
              BOOST_LOG(error) << "[portalgrab] Unknown exception caught in xdg_worker"sv;
            }
          });
        });
        return false;
      default:
        return false;
    }
  }
#endif

#ifdef SUNSHINE_BUILD_KWIN
  std::vector<std::string> kwin_display_names();
  std::shared_ptr<display_t> kwin_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  bool verify_kwin() {
    return !kwin_display_names().empty();
  }
#endif

  /**
   * @brief List display names accepted by the selected capture backend.
   */
  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
#ifdef SUNSHINE_BUILD_CUDA
    // display using NvFBC only supports mem_type_e::cuda
    if (sources[source::NVFBC] && hwdevice_type == mem_type_e::cuda) {
      return nvfbc_display_names();
    }
#endif
#ifdef SUNSHINE_BUILD_WAYLAND
    if (sources[source::WAYLAND]) {
      return wl_display_names();
    }
#endif
#ifdef SUNSHINE_BUILD_DRM
    if (sources[source::KMS]) {
      return kms_display_names(hwdevice_type);
    }
#endif
#ifdef SUNSHINE_BUILD_X11
    if (sources[source::X11]) {
      return x11_display_names();
    }
#endif
#ifdef SUNSHINE_BUILD_PORTAL
    if (sources[source::PORTAL]) {
      return portal_display_names(true);
    }
#endif
#ifdef SUNSHINE_BUILD_KWIN
    if (sources[source::KWIN]) {
      return kwin_display_names();
    }
#endif
    return {};
  }

  /**
   * @brief Report whether encoder backends should be probed again before streaming.
   *
   * @return Always `true` because Linux GPU changes are not tracked by this backend.
   */
  bool needs_encoder_reenumeration() {
    // Only re-probe if the GPU render device changed (hotplug, driver reload).
    // Full re-probing on every reconnect leaks ~20 MB due to FFmpeg CBS
    // allocations during HEVC/AV1 codec validation.
    static std::string last_render_device;
    auto current = platf::resolve_render_device();
    if (current == last_render_device) {
      return false;
    }
    last_render_device = current;
    return true;
  }

  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    // Please ensure that KMS followed by CUDA remains at the top so that we can
    // drop DRM worker privileges once neither backend requires it.

#ifdef SUNSHINE_BUILD_DRM
    if (sources[source::KMS]) {
      BOOST_LOG(info) << "Screencasting with KMS"sv;
      return kms_display(hwdevice_type, display_name, config);
    }
#endif

#ifdef SUNSHINE_BUILD_CUDA
    if (sources[source::NVFBC] && hwdevice_type == mem_type_e::cuda) {
      BOOST_LOG(info) << "Screencasting with NvFBC"sv;
      return nvfbc_display(hwdevice_type, display_name, config);
    }
#endif

#ifdef SUNSHINE_BUILD_DRM
    // Drop all DRM worker thread privileges if not needed for this process's lifetime.
    platf::kms::drop_drm_worker_privileges();
#endif

#ifdef SUNSHINE_BUILD_WAYLAND
    if (sources[source::WAYLAND]) {
      BOOST_LOG(info) << "Screencasting with Wayland's protocol"sv;
      return wl_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef SUNSHINE_BUILD_X11
    if (sources[source::X11]) {
      BOOST_LOG(info) << "Screencasting with X11"sv;
      return x11_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef SUNSHINE_BUILD_PORTAL
    if (sources[source::PORTAL]) {
      BOOST_LOG(info) << "Screencasting with XDG portal"sv;
      return portal_display(hwdevice_type, display_name, config);
    }
#endif
#ifdef SUNSHINE_BUILD_KWIN
    if (sources[source::KWIN]) {
      BOOST_LOG(info) << "Screencasting with KWin ScreenCast"sv;
      return kwin_display(hwdevice_type, display_name, config);
    }
#endif

    return nullptr;
  }

  /**
   * @brief Initialize the Linux high-precision timer file descriptor.
   */
  std::unique_ptr<deinit_t> init() {
    // enable low latency mode for AMD
    // https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30039
    lizardbyte::common::set_env("AMD_DEBUG", "lowlatencyenc");

    // Overriding the video_encode extension can cause corrupt video due to outdated FW. Override
    // only if the user explicitly selects Vulkan so that the autoprobe path will select VAAPI on
    // such systems.
    // Ref: https://gitlab.freedesktop.org/mesa/mesa/-/work_items/16457
    if (config::video.encoder == "vulkan") {
      // enable Vulkan video extensions for AMD RADV
      lizardbyte::common::set_env("RADV_PERFTEST", "video_encode");
      // Above is deprecated on Mesa 26.1+ and replaced by (keep both to ensure best compatibility):
      lizardbyte::common::append_env("RADV_EXPERIMENTAL", "video_encode", ",");
    }

    // These are allowed to fail.
    gbm::init();

    window_system = window_system_e::NONE;
#ifdef SUNSHINE_BUILD_WAYLAND
    if (std::string v; lizardbyte::common::get_env("WAYLAND_DISPLAY", v)) {
      window_system = window_system_e::WAYLAND;
    }
#endif
#if defined(SUNSHINE_BUILD_X11) || defined(SUNSHINE_BUILD_CUDA)
    if (std::string v; lizardbyte::common::get_env("DISPLAY", v) && window_system != window_system_e::WAYLAND) {
      if (lizardbyte::common::get_env("WAYLAND_DISPLAY", v)) {
        BOOST_LOG(warning) << "Wayland detected, yet sunshine will use X11 for screencasting, screencasting will only work on XWayland applications"sv;
      }

      window_system = window_system_e::X11;
    }
#endif

    // Avoid mutating config directly if Portal needs to run in fallback capture mode.
    std::string selected_capture = config::video.capture;

    // When Portal is explicitly selected, probe it first so other capture methods can be considered for fallback capture.
#ifdef SUNSHINE_BUILD_PORTAL
    bool portal_available = false;
    if (selected_capture == "portal") {
      portal_available = verify_portal();
      if (!portal_available) {
        // Continue probing for fallback capture methods.
        selected_capture.clear();
      }
    }
#endif
#ifdef SUNSHINE_BUILD_CUDA
    if (((selected_capture.empty() && sources.none()) || selected_capture == "nvfbc") && verify_nvfbc()) {
      sources[source::NVFBC] = true;
    }
#endif
#ifdef SUNSHINE_BUILD_WAYLAND
    if (((selected_capture.empty() && sources.none()) || selected_capture == "wlr") && verify_wl()) {
      sources[source::WAYLAND] = true;
    }
#endif
#ifdef SUNSHINE_BUILD_DRM
    if (((selected_capture.empty() && sources.none()) || selected_capture == "kms") && verify_kms()) {
      sources[source::KMS] = true;
    }
#endif
#ifdef SUNSHINE_BUILD_X11
    // We enumerate this capture backend regardless of other suitable sources,
    // since it may be needed as a NvFBC fallback for software encoding on X11.
    if ((selected_capture.empty() || selected_capture == "x11") && verify_x11()) {
      sources[source::X11] = true;
    }
#endif
#ifdef SUNSHINE_BUILD_KWIN
    if (((selected_capture.empty() && sources.none()) || selected_capture == "kwin") && verify_kwin()) {
      sources[source::KWIN] = true;
    }
#endif
#ifdef SUNSHINE_BUILD_PORTAL
    if (portal_available || (config::video.capture.empty() && sources.none() && verify_portal())) {
      sources[source::PORTAL] = true;
    }
#endif

    if (sources.none()) {
      BOOST_LOG(error) << "Unable to initialize capture method"sv;
      log_missing_graphical_session_hint();
      return nullptr;
    }

    if (!gladLoaderLoadEGL(NULL)) {
      BOOST_LOG(error) << "Failed to load EGL library symbols"sv;
      return nullptr;
    }

#ifdef SUNSHINE_BUILD_PORTAL
    class deinit_t: public platf::deinit_t {
    public:
      /**
       * @brief Handle xdg_worker thread cleanup in destructor.
       */
      ~deinit_t() override {
        try {
          if (portal::xdg_worker.joinable()) {
            // Make sure the worker's response loop sees shutdown before we block on join().
            if (mail::man) {
              mail::man->event<bool>(mail::shutdown)->raise(true);
            }
            portal::xdg_worker.join();
          }
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "[portalgrab] Exception while joining xdg_worker: "sv << err.what();
        }
      }
    };
#endif

    return std::make_unique<deinit_t>();
  }

  /**
   * @brief Explicitly linux high-precision timer implementation backed by `timerfd`.
   */
  class linux_high_precision_timer: public high_precision_timer {
  public:
    void sleep_for(const std::chrono::nanoseconds &duration) override {
      std::this_thread::sleep_for(duration);
    }

    explicit operator bool() override {
      return true;
    }
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<linux_high_precision_timer>();
  }

  /**
   * @brief Find the DRM render node associated with the active display.
   *
   * @return Render-node path, or an empty string when no matching node is found.
   */
  std::string find_render_node_with_display() {
#ifdef SUNSHINE_BUILD_DRM
    auto *dir = opendir("/dev/dri");
    if (!dir) {
      return {};
    }

    std::string result;
    while (auto *entry = readdir(dir)) {
      if (strncmp(entry->d_name, "card", 4) != 0 || !isdigit(entry->d_name[4])) {
        continue;
      }

      std::string path = std::string("/dev/dri/") + entry->d_name;
      int fd = open(path.c_str(), O_RDWR);
      if (fd < 0) {
        continue;
      }

      auto *res = drmModeGetResources(fd);
      if (res) {
        for (int i = 0; i < res->count_connectors && result.empty(); i++) {
          auto *conn = drmModeGetConnector(fd, res->connectors[i]);
          if (conn) {
            if (conn->connection == DRM_MODE_CONNECTED) {
              char *render = drmGetRenderDeviceNameFromFd(fd);
              if (render) {
                result = render;
                free(render);
              }
            }
            drmModeFreeConnector(conn);
          }
        }
        drmModeFreeResources(res);
      }
      close(fd);
      if (!result.empty()) {
        break;
      }
    }
    closedir(dir);
    return result;
#else
    return {};
#endif
  }

  std::string resolve_render_device() {
    if (!config::video.adapter_name.empty()) {
      return config::video.adapter_name;
    }
    auto detected = find_render_node_with_display();
    return detected.empty() ? "/dev/dri/renderD128" : detected;
  }

#if !defined(__FreeBSD__)
  static constexpr cap_value_t FULL_CAPS[] = {CAP_SYS_ADMIN, CAP_SYS_NICE};
  static constexpr cap_value_t ADMIN_CAPS[] = {CAP_SYS_ADMIN};

  constexpr std::span<const cap_value_t> ELEVATED_PRIVILEGES_FULL {FULL_CAPS};  ///< Protocol or platform constant for elevated privileges full.
  constexpr std::span<const cap_value_t> ELEVATED_PRIVILEGES_ADMIN {ADMIN_CAPS};  ///< Protocol or platform constant for elevated privileges admin.
#endif

  bool has_elevated_privileges(bool all_caps) {
#if !defined(__FreeBSD__)
    const auto caps_to_check = all_caps ? ELEVATED_PRIVILEGES_FULL : ELEVATED_PRIVILEGES_ADMIN;
    const cap_t caps = cap_get_proc();
    if (!caps) {
      BOOST_LOG(error) << "[misc] has_elevated_privileges failed to get process capabilities."sv;
      return false;
    }
    for (const auto c : caps_to_check) {
      cap_flag_value_t cap_flags_value;
      cap_get_flag(caps, c, CAP_EFFECTIVE, &cap_flags_value);
      if (cap_flags_value == CAP_SET) {
        BOOST_LOG(debug) << "[misc] has_elevated_privileges found effective cap:"sv << c;
        return true;
      }
    }
    for (const auto c : caps_to_check) {
      cap_flag_value_t cap_flags_value;
      cap_get_flag(caps, c, CAP_PERMITTED, &cap_flags_value);
      if (cap_flags_value == CAP_SET) {
        BOOST_LOG(debug) << "[misc] has_elevated_privileges found permitted cap:"sv << c;
        return true;
      }
    }
    cap_free(caps);
#endif
    return false;
  }

  void drop_elevated_privileges(bool all_caps) {
#if !defined(__FreeBSD__)
    bool failed = false;
    const auto caps_to_drop = all_caps ? ELEVATED_PRIVILEGES_FULL : ELEVATED_PRIVILEGES_ADMIN;
    const cap_t caps = cap_get_proc();
    if (!caps) {
      BOOST_LOG(error) << "[misc] drop_elevated_privileges failed to get process capabilities"sv;
      return;
    }

    cap_set_flag(caps, CAP_EFFECTIVE, caps_to_drop.size(), caps_to_drop.data(), CAP_CLEAR);
    cap_set_flag(caps, CAP_PERMITTED, caps_to_drop.size(), caps_to_drop.data(), CAP_CLEAR);

    if (cap_set_proc(caps) != 0) {
      BOOST_LOG(error) << "[misc] drop_elevated_privileges failed to prune capabilities: "sv << std::strerror(errno);
      failed = true;
    }
    cap_free(caps);

    // Reset dumpable AFTER the caps have been pruned to ensure /proc/pid/root is accessible.
    if (prctl(PR_SET_DUMPABLE, 1) != 0) {
      BOOST_LOG(error) << "[misc] drop_elevated_privileges failed to set PR_SET_DUMPABLE: "sv << std::strerror(errno);
      failed = true;
    }
    if (!failed) {
      BOOST_LOG(info) << "[misc] drop_elevated_privileges succeeded in dropping capabilities"sv;
    }
#endif
  }
}  // namespace platf
