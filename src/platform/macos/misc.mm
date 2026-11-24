/**
 * @file src/platform/macos/misc.mm
 * @brief Miscellaneous definitions for macOS platform.
 */

// Required for IPV6_PKTINFO with Darwin headers
#ifndef __APPLE_USE_RFC_3542  // NOLINT(bugprone-reserved-identifier)
  /**
   * @def __APPLE_USE_RFC_3542
   * @brief Macro for APPLE USE RFC 3542.
   */
  #define __APPLE_USE_RFC_3542 1
#endif

// standard includes
#include <algorithm>
#include <future>
#include <memory>

// platform includes
#include <AppKit/AppKit.h>
#include <arpa/inet.h>
#include <AVFoundation/AVFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <Foundation/Foundation.h>
#include <ifaddrs.h>
#include <mach-o/dyld.h>
#include <net/if_dl.h>
#include <pwd.h>
#include <sys/qos.h>
#include <UserNotifications/UserNotifications.h>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>

// local includes
#include "misc.h"
#include "src/compat/boost_process.h"
#include "src/config.h"
#include "src/entry_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;
namespace fs = std::filesystem;
namespace bp = boost::process::v1;

namespace platf {

// Even though the following two functions are available starting in macOS 10.15, they weren't
// actually in the Mac SDK until Xcode 12.2, the first to include the SDK for macOS 11
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 110000  // __MAC_11_0
  // If they're not in the SDK then we can use our own function definitions.
  // Need to use weak import so that this will link in macOS 10.14 and earlier
  /**
   * @brief Query macOS screen-capture permission without prompting the user.
   *
   * @return True when screen-capture permission is granted.
   */
  extern "C" bool CGPreflightScreenCaptureAccess(void) __attribute__((weak_import));
  /**
   * @brief Request macOS screen-capture permission from the user.
   *
   * @return True when screen-capture permission is granted.
   */
  extern "C" bool CGRequestScreenCaptureAccess(void) __attribute__((weak_import));
#endif

  namespace {
    auto screen_capture_allowed = std::atomic<bool> {false};
    NSString *const screen_recording_state_key = @"screenRecordingPermissionState";  ///< Last screen recording request state for this user.
    NSString *const input_post_event_state_key = @"inputPostEventPermissionState";  ///< Last keyboard and mouse request state for this user.
    NSString *const system_audio_requested_key = @"systemAudioStartupRequested";  ///< Whether Sunshine already started a startup tap.

    /**
     * @brief Check screen recording without showing a macOS prompt.
     *
     * @return True when capture is allowed or the old OS has no privacy gate.
     */
    bool screen_recording_granted() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#pragma clang diagnostic ignored "-Wtautological-pointer-compare"
      return ![[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:((NSOperatingSystemVersion) {10, 15, 0})] ||
             CGPreflightScreenCaptureAccess == nullptr || CGPreflightScreenCaptureAccess();
#pragma clang diagnostic pop
    }

    /**
     * @brief Query notification authorization without holding a callback stack frame alive.
     *
     * @return Authorization state name for the Web UI.
     */
    std::string notification_status() {
      auto result = std::make_shared<std::promise<UNAuthorizationStatus>>();
      auto future = result->get_future();
      [[UNUserNotificationCenter currentNotificationCenter] getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
        result->set_value(settings.authorizationStatus);
      }];
      if (future.wait_for(2s) != std::future_status::ready) {
        return "unknown";
      }
      switch (future.get()) {
        case UNAuthorizationStatusAuthorized:
        case UNAuthorizationStatusProvisional:
          return "granted";
        case UNAuthorizationStatusDenied:
          return "denied";
        case UNAuthorizationStatusNotDetermined:
          return "not_determined";
      }
      return "unknown";
    }

    /**
     * @brief Open a fixed macOS privacy settings pane.
     *
     * @param pane Privacy pane anchor, selected from known Sunshine permissions.
     * @return True when macOS accepted the settings URL.
     */
    bool open_privacy_settings(NSString *pane) {
      NSString *address = [@"x-apple.systempreferences:com.apple.preference.security?" stringByAppendingString:pane];
      return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:address]] == YES;
    }

    /**
     * @brief Request a CoreGraphics permission or open Settings when it was previously requested.
     *
     * @param startup Whether Sunshine is performing automatic startup checks.
     * @param state_key User defaults key recording this permission's previous state.
     * @param granted Whether the native preflight check reports access.
     * @param pane Settings pane to open for manual authorization.
     * @param request Native function that initiates the initial authorization request.
     * @param retry_manual_request Whether a user action should retry the native request and open Settings.
     * @return True when access exists or a native action was started.
     */
    bool request_coregraphics_permission(bool startup, NSString *state_key, bool granted, NSString *pane, bool (*request)(), bool retry_manual_request) {
      NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
      const auto previous_state = static_cast<int>([defaults integerForKey:state_key]);
      const auto action = permission_request_action(granted, previous_state, startup, retry_manual_request);
      if (granted) {
        [defaults setInteger:2 forKey:state_key];
      }
      if (action == permission_request_action_t::none) {
        return granted;
      }
      if (action == permission_request_action_t::settings) {
        [defaults setInteger:1 forKey:state_key];
        return open_privacy_settings(pane);
      }
      [defaults setInteger:1 forKey:state_key];
      if (request()) {
        [defaults setInteger:2 forKey:state_key];
        if (!startup && retry_manual_request) {
          open_privacy_settings(pane);
        }
        return true;
      }
      return open_privacy_settings(pane);
    }

    /**
     * @brief Request screen capture access without repeating an earlier startup prompt.
     *
     * @param startup Whether this is an automatic startup request.
     * @return True when access exists or the request was opened.
     */
    bool request_screen_recording(bool startup) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
      return request_coregraphics_permission(startup, screen_recording_state_key, screen_recording_granted(), @"Privacy_ScreenCapture", CGRequestScreenCaptureAccess, false);
#pragma clang diagnostic pop
    }

    /**
     * @brief Request Post Event access for libvirtualhid keyboard and mouse input.
     *
     * @param startup Whether this is an automatic startup request.
     * @return True when access exists or the request was opened.
     */
    bool request_input_post_event(bool startup) {
      return request_coregraphics_permission(startup, input_post_event_state_key, CGPreflightPostEventAccess(), @"Privacy_Accessibility", CGRequestPostEventAccess, true);
    }
  }  // namespace

  permission_request_action_t permission_request_action(bool granted, int previous_state, bool startup, bool retry_manual_request) {
    if (granted) {
      return permission_request_action_t::none;
    }
    if (!startup && retry_manual_request) {
      return permission_request_action_t::prompt;
    }
    if (previous_state <= 0) {
      return permission_request_action_t::prompt;
    }
    if (previous_state == 2 || !startup) {
      return permission_request_action_t::settings;
    }
    return permission_request_action_t::none;
  }

  bool supports_local_network_privacy(int major_version) {
    return major_version >= 15;
  }

  bool should_request_startup_system_audio_permission(bool has_custom_sink, bool previously_requested) {
    return !has_custom_sink && !previously_requested;
  }

  std::vector<permission_status_t> get_permission_statuses() {
    const auto microphone_authorization = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    std::string microphone_status = "unknown";
    switch (microphone_authorization) {
      case AVAuthorizationStatusAuthorized:
        microphone_status = "granted";
        break;
      case AVAuthorizationStatusDenied:
      case AVAuthorizationStatusRestricted:
        microphone_status = "denied";
        break;
      case AVAuthorizationStatusNotDetermined:
        microphone_status = "not_determined";
        break;
    }

    const auto screen_granted = screen_recording_granted();
    const auto input_granted = CGPreflightPostEventAccess();
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (screen_granted) {
      [defaults setInteger:2 forKey:screen_recording_state_key];
    }
    if (input_granted) {
      [defaults setInteger:2 forKey:input_post_event_state_key];
    }

    std::vector<permission_status_t> statuses {
      {"screen_recording", screen_granted ? "granted" : "denied", true, true, true},
      {"input", input_granted ? "granted" : "denied", config::input.keyboard || config::input.mouse, true, true},
      {"microphone", microphone_status, !config::audio.sink.empty(), true, true},
      {"system_audio", "on_use", config::audio.sink.empty(), false, true},
    };
    if (supports_local_network_privacy(static_cast<int>([[NSProcessInfo processInfo] operatingSystemVersion].majorVersion))) {
      statuses.push_back({"local_network", "on_use", true, false, true});
    }
    statuses.push_back({"notifications", notification_status(), false, true, true});
    return statuses;
  }

  bool should_request_startup_permission(const permission_status_t &permission, bool notifications_enabled) {
    if (permission.id == "notifications") {
      return notifications_enabled && permission.status == "not_determined";
    }
    if (!permission.required) {
      return false;
    }
    if (permission.id == "microphone") {
      return permission.status == "not_determined" || permission.status == "denied";
    }
    return (permission.id == "screen_recording" || permission.id == "input") &&
           (permission.status == "not_determined" || permission.status == "denied");
  }

  bool request_permission(std::string_view id) {
    if (id == "screen_recording") {
      return request_screen_recording(false);
    }
    if (id == "input") {
      return request_input_post_event(false);
    }
    if (id == "microphone") {
      const auto authorization = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
      if (authorization == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
        return true;
      }
      return authorization == AVAuthorizationStatusAuthorized || open_privacy_settings(@"Privacy_Microphone");
    }
    if (id == "notifications") {
      if (notification_status() == "denied") {
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.notifications"]] == YES;
      }
      [[UNUserNotificationCenter currentNotificationCenter] requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                                                          completionHandler:^(BOOL, NSError *) {}];
      return true;
    }
    if (id == "system_audio") {
      const bool settings_opened = open_privacy_settings(@"Privacy_ScreenCapture");
      if (!request_system_audio_permission()) {
        BOOST_LOG(warning) << "System audio recording permission or tap setup is unavailable"sv;
      }
      return settings_opened;
    }
    if (id == "local_network") {
      if (!supports_local_network_privacy(static_cast<int>([[NSProcessInfo processInfo] operatingSystemVersion].majorVersion))) {
        return false;
      }
      return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension"]] == YES;
    }
    return false;
  }

  void request_startup_permissions(bool notifications_enabled) {
    for (const auto &permission : get_permission_statuses()) {
      if (should_request_startup_permission(permission, notifications_enabled)) {
        if (permission.id == "screen_recording") {
          request_screen_recording(true);
        } else if (permission.id == "input") {
          request_input_post_event(true);
        } else {
          request_permission(permission.id);
        }
      }
    }
  }

  void request_startup_system_audio_permission() {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (!should_request_startup_system_audio_permission(!config::audio.sink.empty(), [defaults boolForKey:system_audio_requested_key])) {
      return;
    }
    [defaults setBool:YES forKey:system_audio_requested_key];
    if (!request_system_audio_permission()) {
      BOOST_LOG(warning) << "System audio recording permission or tap setup is unavailable"sv;
    }
  }

  // Return whether screen capture is allowed for this process.
  /**
   * @brief Check whether screen capture allowed.
   */
  bool is_screen_capture_allowed() {
    return screen_capture_allowed;
  }

  std::unique_ptr<deinit_t> init() {
    if (!screen_recording_granted()) {
      BOOST_LOG(error) << "No screen capture permission!"sv;
      BOOST_LOG(error) << "Please activate Sunshine in System Settings -> Privacy & Security -> Screen & System Audio Recording"sv;
      return nullptr;
    }
    // Record that we determined that we have the screen capture permission.
    screen_capture_allowed = true;
    return std::make_unique<deinit_t>();
  }

  fs::path appdata() {
    const char *homedir;
    if ((homedir = getenv("HOME")) == nullptr) {
      homedir = getpwuid(geteuid())->pw_dir;
    }

    return fs::path {homedir} / ".config/sunshine"sv;
  }

  /**
   * @brief XDG Portal token path (unused on MacOS).
   *
   * @return Path of portal_token in appdata path.
   */
  std::filesystem::path get_xdg_restore_token_path() {
    return appdata() / "portal_token";
  }

  using ifaddr_t = util::safe_ptr<ifaddrs, freeifaddrs>;

  ifaddr_t get_ifaddrs() {
    ifaddrs *p {nullptr};

    getifaddrs(&p);

    return ifaddr_t {p};
  }

  std::string from_sockaddr(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};

    auto family = ip_addr->sa_family;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    }

    return std::string {data};
  }

  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};

    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
      port = ((sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
      port = ((sockaddr_in *) ip_addr)->sin_port;
    }

    return {port, std::string {data}};
  }

  std::string get_mac_address(const std::string_view &address) {
    auto ifaddrs = get_ifaddrs();

    for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
      if (pos->ifa_addr && address == from_sockaddr(pos->ifa_addr)) {
        BOOST_LOG(verbose) << "Looking for MAC of "sv << pos->ifa_name;

        struct ifaddrs *ifap, *ifaptr;
        unsigned char *ptr;
        std::string mac_address;

        if (getifaddrs(&ifap) == 0) {
          for (ifaptr = ifap; ifaptr != nullptr; ifaptr = (ifaptr)->ifa_next) {
            if (!strcmp((ifaptr)->ifa_name, pos->ifa_name) && (((ifaptr)->ifa_addr)->sa_family == AF_LINK)) {
              ptr = (unsigned char *) LLADDR((struct sockaddr_dl *) (ifaptr)->ifa_addr);
              char buff[100];

              snprintf(buff, sizeof(buff), "%02x:%02x:%02x:%02x:%02x:%02x", *ptr, *(ptr + 1), *(ptr + 2), *(ptr + 3), *(ptr + 4), *(ptr + 5));
              mac_address = buff;
              break;
            }
          }

          freeifaddrs(ifap);

          if (ifaptr != nullptr) {
            BOOST_LOG(verbose) << "Found MAC of "sv << pos->ifa_name << ": "sv << mac_address;
            return mac_address;
          }
        }
      }
    }

    BOOST_LOG(warning) << "Unable to find MAC address for "sv << address;
    return "00:00:00:00:00:00"s;
  }

  bp::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
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
    boost::filesystem::path working_dir;
    std::string cmd = R"(open ")" + url + R"(")";

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

  void adjust_thread_priority(thread_priority_e priority) {
    qos_class_t mac_priority;

    switch (priority) {
      case thread_priority_e::low:
        mac_priority = QOS_CLASS_UTILITY;
        break;
      case thread_priority_e::normal:
        mac_priority = QOS_CLASS_DEFAULT;
        break;
      case thread_priority_e::high:
        mac_priority = QOS_CLASS_USER_INITIATED;
        break;
      case thread_priority_e::critical:
        mac_priority = QOS_CLASS_USER_INTERACTIVE;
        break;
      default:
        BOOST_LOG(error) << "Unknown thread priority: "sv << (int) priority;
        return;
    }

    // https://github.com/apple/darwin-libpthread/blob/main/include/sys/qos.h
    pthread_set_qos_class_self_np(mac_priority, 0);
  }

  void set_thread_name(std::string_view name) {
    std::string thread_name {name};
    pthread_setname_np(thread_name.c_str());
  }

  void enable_mouse_keys() {
    // Unimplemented
  }

  void streaming_will_start() {
    // Nothing to do
  }

  void streaming_will_stop() {
    // Nothing to do
  }

  static pid_t g_restart_child_pid = 0;  ///< PID of the restarted child process for signal forwarding.

  /**
   * @brief Forward a signal to the restarted child process.
   *
   * This handler is installed in the parent (supervisor) process after forking
   * the new Sunshine instance. It ensures that signals like SIGINT (Ctrl+C)
   * are delivered to the child.
   *
   * @param sig The signal number to forward.
   */
  static void forward_signal_to_child(int sig) {
    if (g_restart_child_pid > 0) {
      kill(g_restart_child_pid, sig);
    }
  }

  /**
   * @brief Request a Sunshine process restart on exit.
   *
   * This is registered as an atexit handler by restart(). It forks a child
   * process with a fresh PID so that macOS WindowServer treats it as a new
   * application (required for the system tray icon to reinitialize).
   *
   * The parent process stays alive as a transparent supervisor: it forwards
   * signals (SIGINT, SIGTERM, SIGHUP) to the child and blocks in waitpid().
   * This keeps the shell tracking the original PID as its foreground job,
   * preserving Ctrl+C and terminal log output.
   */
  void restart_on_exit() {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) < 0) {
      BOOST_LOG(fatal) << "NSGetExecutablePath() failed: "sv << errno;
      return;
    }

    // ASIO doesn't use O_CLOEXEC, so we have to close all fds ourselves.
    int openmax = (int) sysconf(_SC_OPEN_MAX);
    for (int fd = STDERR_FILENO + 1; fd < openmax; fd++) {
      close(fd);
    }

    // Fork a child process to get a fresh PID.
    // A new PID is required on macOS because WindowServer associates GUI state
    // (tray icons, activation policy) with the PID. After execv with the same PID,
    // WindowServer retains stale state and silently refuses to show new tray icons.
    pid_t child = fork();
    if (child < 0) {
      BOOST_LOG(fatal) << "fork() failed: "sv << errno;
      return;
    }

    if (child == 0) {
      // Child: create a new process group so that the parent's signal
      // forwarding targets only this child, not the parent itself.
      setpgid(0, 0);

      // Replace this child with the new Sunshine instance
      execv(executable, lifetime::get_argv());

      // If execv fails, exit the child immediately without running atexit handlers
      _exit(1);
    }

    // Parent: become a transparent supervisor.
    // The parent stays alive so the shell continues to track it as the
    // foreground job, keeping Ctrl+C and terminal output working.
    g_restart_child_pid = child;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal_to_child;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    int status;
    waitpid(child, &status, 0);

    // Exit immediately without running additional atexit handlers or
    // static destructors. The child has already taken over.
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
  }

  void restart() {
    // Gracefully clean up and restart ourselves instead of exiting
    atexit(restart_on_exit);
    lifetime::exit_sunshine(0, true);
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    if (killpg((pid_t) native_handle, SIGTERM) == 0 || errno == ESRCH) {
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

  struct sockaddr_in to_sockaddr(boost::asio::ip::address_v4 address, uint16_t port) {
    struct sockaddr_in saddr_v4 = {};

    saddr_v4.sin_family = AF_INET;
    saddr_v4.sin_port = htons(port);

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v4.sin_addr, addr_bytes.data(), sizeof(saddr_v4.sin_addr));

    return saddr_v4;
  }

  struct sockaddr_in6 to_sockaddr(boost::asio::ip::address_v6 address, uint16_t port) {
    struct sockaddr_in6 saddr_v6 = {};

    saddr_v6.sin6_family = AF_INET6;
    saddr_v6.sin6_port = htons(port);
    saddr_v6.sin6_scope_id = address.scope_id();

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v6.sin6_addr, addr_bytes.data(), sizeof(saddr_v6.sin6_addr));

    return saddr_v6;
  }

  bool send_batch(const batched_send_info_t &send_info) {
    // Fall back to unbatched send calls
    return false;
  }

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
      char buf[std::max(CMSG_SPACE(sizeof(struct in_pktinfo)), CMSG_SPACE(sizeof(struct in6_pktinfo)))];
      struct cmsghdr alignment;
    } cmbuf {};

    socklen_t cmbuflen = 0;

    msg.msg_control = cmbuf.buf;
    msg.msg_controllen = sizeof(cmbuf.buf);

    auto pktinfo_cm = CMSG_FIRSTHDR(&msg);
    if (send_info.source_address.is_v6()) {
      struct in6_pktinfo pktInfo {};

      struct sockaddr_in6 saddr_v6 = to_sockaddr(send_info.source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IPV6;
      pktinfo_cm->cmsg_type = IPV6_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    } else {
      struct in_pktinfo pktInfo {};

      struct sockaddr_in saddr_v4 = to_sockaddr(send_info.source_address.to_v4(), 0);
      pktInfo.ipi_spec_dst = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IP;
      pktinfo_cm->cmsg_type = IP_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    }

    struct iovec iovs[2] = {};
    int iovlen = 0;
    if (send_info.header) {
      iovs[iovlen].iov_base = (void *) send_info.header;
      iovs[iovlen].iov_len = send_info.header_size;
      iovlen++;
    }
    iovs[iovlen].iov_base = (void *) send_info.payload;
    iovs[iovlen].iov_len = send_info.payload_size;
    iovlen++;

    msg.msg_iov = iovs;
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
  static std::atomic<int> qos_ref_count = 0;

  /**
   * @brief Owns platform QoS state that is restored during cleanup.
   */
  class qos_t: public deinit_t {
  public:
    /**
     * @brief Apply macOS socket QoS settings for scoped cleanup.
     *
     * @param sockfd Native socket descriptor whose options are updated.
     * @param options Request options or socket options to apply.
     */
    qos_t(int sockfd, std::vector<std::tuple<int, int, int>> options):
        sockfd(sockfd),
        options(options) {
      qos_ref_count++;
    }

    virtual ~qos_t() {
      if (--qos_ref_count == 0) {
        for (const auto &tuple : options) {
          auto reset_val = std::get<2>(tuple);
          if (setsockopt(sockfd, std::get<0>(tuple), std::get<1>(tuple), &reset_val, sizeof(reset_val)) < 0) {
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
    int sockfd = (int) native_socket;
    std::vector<std::tuple<int, int, int>> reset_options;

    // We can use SO_NET_SERVICE_TYPE to set link-layer prioritization without DSCP tagging
    int service_type = 0;
    switch (data_type) {
      case qos_data_type_e::video:
        service_type = NET_SERVICE_TYPE_VI;
        break;
      case qos_data_type_e::audio:
        service_type = NET_SERVICE_TYPE_VO;
        break;
      default:
        BOOST_LOG(error) << "Unknown traffic type: "sv << (int) data_type;
        break;
    }

    if (service_type) {
      if (setsockopt(sockfd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service_type, sizeof(service_type)) == 0) {
        // Reset SO_NET_SERVICE_TYPE to best-effort when QoS is disabled
        reset_options.emplace_back(std::make_tuple(SOL_SOCKET, SO_NET_SERVICE_TYPE, NET_SERVICE_TYPE_BE));
      } else {
        BOOST_LOG(error) << "Failed to set SO_NET_SERVICE_TYPE: "sv << errno;
      }
    }

    if (dscp_tagging) {
      int level;
      int option;
      if (address.is_v6()) {
        level = IPPROTO_IPV6;
        option = IPV6_TCLASS;
      } else {
        level = IPPROTO_IP;
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
          BOOST_LOG(error) << "Unknown traffic type: "sv << (int) data_type;
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

  /**
   * @brief macOS high-precision timer implementation backed by a worker thread.
   */
  class macos_high_precision_timer: public high_precision_timer {
  public:
    void sleep_for(const std::chrono::nanoseconds &duration) override {
      std::this_thread::sleep_for(duration);
    }

    /**
     * @brief Explicitly report that the thread-based timer is always available.
     *
     * @return True because sleeping needs no additional timer resource.
     */
    explicit operator bool() override {
      return true;
    }
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<macos_high_precision_timer>();
  }

  std::string resolve_render_device() {
    return {};
  }
}  // namespace platf

namespace dyn {
  void *handle(const std::vector<const char *> &libs) {
    void *handle;

    for (auto lib : libs) {
      handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL);
      if (handle) {
        return handle;
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

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict) {
    int err = 0;
    for (auto &func : funcs) {
      TUPLE_2D_REF(fn, name, func);

      *fn = (void (*)()) dlsym(handle, name);

      if (!*fn && strict) {
        BOOST_LOG(error) << "Couldn't find function: "sv << name;

        err = -1;
      }
    }

    return err;
  }
}  // namespace dyn
