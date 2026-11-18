/**
 * @file src/platform/linux/kwingrab.cpp
 * @brief KWin direct ScreenCast capture via zkde_screencast_unstable_v1 Wayland protocol.
 *
 * Bypasses xdg-desktop-portal entirely. Sunshine connects directly to KWin's
 * Wayland protocol to obtain a PipeWire node_id, then streams frames via PipeWire.
 *
 * Chain: KWin -> Wayland kde_screencast -> PipeWire -> Sunshine
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <pwd.h>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

// lib includes
#include <lizardbyte/common/env.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// generated protocol header
#include <kde-output-order-v1.h>
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "pipewire.cpp"
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace kwin {
  /**
   * KWin Wayland ScreenCast permissions
   *
   * To have access to zkde_screencast_unstable_v1 KWin checks for a .desktop file with
   * X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1 and the current executable name
   * in the Exec= parameter.
   */
  class screencast_permission_helper_t {
  public:
    /**
     * @brief Check whether permission system deactivated.
     *
     * @return True when KWin reports that the permission system is disabled.
     */
    static bool is_permission_system_deactivated() {
      return lizardbyte::common::get_env("KWIN_WAYLAND_NO_PERMISSION_CHECKS") == "1";
    }

    /**
     * @brief Configure the KWin screencast session.
     */
    static void setup() {
      if (initialized) {
        return;
      }
      auto filenameprefix = std::format("{}.kwin", PROJECT_FQDN);
      auto executablepath = get_executable_full_path();

      // System: Check system XDG applications for permission (usually installed with Sunshine)
      if (check_kwin_system_permissions(filenameprefix, executablepath)) {
        create_file = false;
        initialized = true;
        return;
      }

      // If we do not have a system permission, check if we need a temporary permission via user's application directory
      if (is_permission_system_deactivated()) {
        BOOST_LOG(info) << "[kwingrab] No permission desktop file necessary. KWin permission system deactivated.";
        create_file = false;
        initialized = true;
        return;
      }

      // User: Check and (if necessary) update user's XDG applications for permission
      auto user_applications = get_xdg_user_applications_path();
      if (user_applications.empty()) {
        BOOST_LOG(error) << "[kwingrab] Failed to determine user application directory. Cannot continue with permission setup.";
        return;
      }
      // Create non-existing application directory so we can write into it
      if (!std::filesystem::exists(user_applications) && !std::filesystem::create_directories(user_applications)) {
        // In case of failure log and return
        BOOST_LOG(error) << "[kwingrab] Failed to create application directory. Cannot continue with permission setup.";
        create_file = false;
        initialized = true;
        return;
      }
      auto user_filepathprefix = (std::filesystem::path(user_applications) / filenameprefix).string();
      for (const auto &path : std::filesystem::directory_iterator(user_applications)) {
        // List existing files for prefix and check if they contain this executable or remove them
        const auto entry = path.path().string();
        if (entry.starts_with(user_filepathprefix)) {
          auto entry_executablepath = get_executable_from_desktop_file(entry);
          if (!entry_executablepath.empty() && entry_executablepath == executablepath) {
            // This entry is exactly the one we need
            BOOST_LOG(debug) << "[kwingrab] Ignoring current temporary KWin wayland permission file: "sv << entry;
            create_file = false;
            continue;
          }
          if (!entry_executablepath.empty() && std::filesystem::exists(entry_executablepath)) {
            // This entry is for another sunshine executable that still exists
            BOOST_LOG(debug) << "[kwingrab] Ignoring other valid temporary KWin wayland permission file: "sv << entry;
            continue;
          }
          if (std::filesystem::remove(path)) {
            BOOST_LOG(info) << "[kwingrab] Removed stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          } else {
            BOOST_LOG(warning) << "[kwingrab] Failed to remove stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          }
        }
      }
      if (create_file) {
        // Generate a unique file identifier based on current unixtime
        auto user_filepathidentifier = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        auto user_filepath = std::format("{}{}.desktop", user_filepathprefix, user_filepathidentifier);
        // Write new file if necessary
        std::ofstream filestream(user_filepath);
        if (filestream.is_open()) {
          filestream << "[Desktop Entry]" << std::endl
                     << "Exec=" << executablepath << std::endl
                     << "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1" << std::endl
                     << "Type=Application" << std::endl
                     << "Name="sv << PROJECT_FQDN << "-kwin-wayland-permission" << std::endl
                     << "Comment=Sunshine KWin screencast permission" << std::endl
                     << "NoDisplay=true" << std::endl;
          filestream.close();
          // Give KWin time to catch up to the new desktop file
          BOOST_LOG(info) << "[kwingrab] Created temporary KWin wayland permission file: "sv << user_filepath << " - Waiting 3 seconds for KDE to pick up new file.";
          std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        } else {
          BOOST_LOG(warning) << "[kwingrab] Failed to open temporary KWin wayland permission file: "sv << user_filepath;
        }
      }

      initialized = true;
    }

    /**
     * @brief Check whether newly initialized.
     *
     * @return True when KWin was initialized during this check.
     */
    static bool is_newly_initialized() {
      return create_file;
    }

  private:
    static inline bool initialized = false;
    static inline bool create_file = true;

    static std::filesystem::path get_home_dir() {
      // Check HOME environment variable
      if (std::string homedir = lizardbyte::common::get_env("HOME"); !homedir.empty()) {
        return homedir;
      }
      // Fall back to home directory from NSS passwd
      // Note: This should be thread-safe as we're always accessing the same entry for Sunshine
      return getpwuid(geteuid())->pw_dir;
    }

    static std::filesystem::path get_xdg_user_applications_path() {
      // Follow the XDG base directory specification for user data home:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      std::filesystem::path xdg_data_home;
      if (std::string dir = lizardbyte::common::get_env("XDG_DATA_HOME"); !dir.empty()) {
        xdg_data_home = std::filesystem::path(dir);
      } else {
        const auto homedir = get_home_dir();
        if (homedir.empty()) {
          return "";
        }
        xdg_data_home = std::filesystem::path(homedir) / ".local"sv / "share"sv;
      }
      return xdg_data_home / "applications";
    }

    static std::string get_executable_full_path() {
      // Adapted from https://linuxvox.com/blog/how-do-i-find-the-location-of-the-executable-in-c/
      constexpr auto path_len = PATH_MAX;  // PATH_MAX is defined in limits.h (e.g., 4096 on Linux)
      auto path_exe = std::make_unique<char[]>(path_len);
      // Read the symlink /proc/self/exe into path_exe
      const ssize_t len = readlink("/proc/self/exe", &path_exe[0], path_len - 1);
      if (len == -1) {
        return "";
      }
      // Return path_exe as a proper std::string with len returned by readlink
      return std::string(path_exe.get(), len);
    }

    static std::string get_executable_from_desktop_file(const std::string &path) {
      if (std::ifstream file(path); file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
          if (line.starts_with("Exec=") && line.length() > 5) {
            return line.substr(5);
          }
        }
      }
      return "";
    }

    static bool check_kwin_system_permissions(const std::string_view &filenameprefix, const std::string_view &executablepath) {
      // Find data dirs to check from XDG_DATA_DIRS
      std::vector<std::string> xdg_data_dirs;
      if (const std::string e = lizardbyte::common::get_env("XDG_DATA_DIRS"); !e.empty()) {
        std::stringstream ss(e);
        std::string item;

        while (getline(ss, item, ':')) {  // : is likely valid for all OSes supported, if a constant is available it should be used instead
          xdg_data_dirs.push_back(item);
        }
      }
      // Use defaults from https://specifications.freedesktop.org/basedir/latest/ if ENV var was empty
      if (xdg_data_dirs.empty()) {
        xdg_data_dirs.emplace_back("/usr/local/share/");
        xdg_data_dirs.emplace_back("/usr/share/");
      }
      // Check for ${filenameprefix}.desktop in each directory
      for (auto const &dir : xdg_data_dirs) {
        std::string filename = std::format("{0}{1}applications{1}{2}.desktop", dir, boost::filesystem::path::preferred_separator, filenameprefix);
        if (std::filesystem::exists(filename)) {
          auto file_executablepath = get_executable_from_desktop_file(filename);
          if (file_executablepath == executablepath) {
            BOOST_LOG(info) << "[kwingrab] Found matching system KWin desktop permission file: "sv << filename;
            return true;
          }
        }
      }
      return false;
    }
  };

  // Output parameters
  /**
   * @brief KWin screencast output name and geometry.
   */
  struct output_parameter_t {
    std::string name;  ///< KWin output name.
    int width = 0;  ///< Output width in pixels.
    int height = 0;  ///< Output height in pixels.
    int pos_x = 0;  ///< Output X position in the compositor layout.
    int pos_y = 0;  ///< Output Y position in the compositor layout.
    // order is needed to get a sorted output list and should be updated before sorting to have current values
    /**
     * @brief Order.
     */
    size_t order = SIZE_MAX;  // Use high number to keep monitors with uninitialized order value to the back
  };

  /**
   * Wayland KDE ScreenCast session
   *
   * Owns its own wl_display connection. Binds zkde_screencast_unstable_v1
   * and wl_output from the registry, then calls stream_output() to start
   * a ScreenCast. Waits for the created(node_id) event from KWin.
   */
  class screencast_t {
  public:
    screencast_t &operator=(screencast_t &&) = delete;  // Do not allow to copying

    ~screencast_t() {
      // Release KDE screencast wayland extensions and reset pointers
      if (kde_screencast_stream_v1_) {
        zkde_screencast_stream_unstable_v1_close(kde_screencast_stream_v1_);
        kde_screencast_stream_v1_ = nullptr;
      }
      if (kde_screencast_v1_) {
        zkde_screencast_unstable_v1_destroy(kde_screencast_v1_);
        kde_screencast_v1_ = nullptr;
      }
      if (kde_output_order) {
        kde_output_order_v1_destroy(kde_output_order);
        kde_output_order = nullptr;
      }

      // Clear output order list
      output_order.clear();
      // Clear current output parameters
      out_params.reset();
      out_params = nullptr;

      // wl_output is owned by the registry, released on disconnect
      // also cleanup associated output parameters and clear output list when done
      for (auto &[output, params] : outputs) {
        wl_output_destroy(output);
        params.reset();
      }
      outputs.clear();

      // Release wayland registry, display and reset pointers
      if (wl_registry) {
        wl_registry_destroy(wl_registry);
        wl_registry = nullptr;
      }
      if (wl_display) {
        wl_display_disconnect(wl_display);
        wl_display = nullptr;
      }
    }

    /**
     * @brief Connect to KWin wayland, enumerate outputs.
     * @param setup_permissions - Try to setup KWin permissions (default: true)
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int init(const bool setup_permissions = true) {
      if (setup_permissions) {
        // Try to set up permissions for zkde_screencast_unstable_v1
        screencast_permission_helper_t::setup();
      }

      std::string wl_name;
      if (!lizardbyte::common::get_env("WAYLAND_DISPLAY", wl_name)) {
        BOOST_LOG(error) << "[kwingrab] WAYLAND_DISPLAY not set"sv;
        return -1;
      }

      wl_display = wl_display_connect(wl_name.c_str());
      if (!wl_display) {
        BOOST_LOG(error) << "[kwingrab] cannot connect to Wayland display: "sv << wl_name;
        return -1;
      }

      wl_registry = wl_display_get_registry(wl_display);
      wl_registry_add_listener(wl_registry, &registry_listener, this);
      wl_display_roundtrip(wl_display);

      // We need a second roundtrip after binding outputs to get wl_output events
      wl_display_roundtrip(wl_display);

      if (!is_kwin_screencasting_available()) {
        BOOST_LOG(debug) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry."sv;
        return -1;
      }

      return 0;
    }

    /**
     * @brief Check if kwin screencasting is currently available
     *
     * @return true if screencast can be started, false otherwise
     */
    bool is_kwin_screencasting_available() const {
      return kde_screencast_v1_ != nullptr;
    }

    /**
     * @brief Generate a sorted list of known output names.
     * @return List of strings with output names to pass to start()
     */
    std::vector<std::string> get_output_names() {
      std::vector<std::shared_ptr<output_parameter_t>> sorted_outputs;
      for (const auto &output_parameter : outputs | std::views::values) {
        output_parameter->order = get_order_for_output_name(output_parameter->name);
        sorted_outputs.emplace_back(output_parameter);
      }
      std::ranges::sort(sorted_outputs, [](const auto &a, const auto &b) {
        return a->order < b->order || a->pos_x < b->pos_x || a->pos_y < b->pos_y;
      });
      std::vector<std::string> output_names;
      for (const auto &output_parameter : sorted_outputs) {
        BOOST_LOG(info) << "[kwingrab] Found output: "sv << output_parameter->name << " order: "sv << output_parameter->order << " position: "sv << output_parameter->pos_x << "x"sv << output_parameter->pos_y << " resolution: "sv << output_parameter->width << "x"sv << output_parameter->height;
        output_names.emplace_back(output_parameter->name);
      }
      return output_names;
    }

    /**
     * @brief Check if KWin is available for potential screencasting
     * @return True if KWin is detected
     */
    bool kwin_available() const {
      // Detect KWin using kde_output_order_v1 extension
      if (kde_output_order) {
        return true;
      }
      return false;
    }

    /**
     * @brief Request a screencast stream.
     * @param output_name Which wl_output to capture.
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int start(const std::string_view &output_name) {
      // Try find correct output by name
      if (outputs.empty()) {
        BOOST_LOG(error) << "[kwingrab] no wl_output found"sv;
        return -1;
      }
      struct wl_output *output = nullptr;
      if (!output_name.empty()) {
        for (auto const &[output_, params_] : outputs) {
          if (params_->name == output_name) {
            output = output_;
            out_params = params_;
          }
        }
      }
      // Fall back to first element from the map in case of error
      if (!output || !out_params) {
        const auto output_ = outputs.begin();
        output = output_->first;
        out_params = output_->second;
      }

      // Request a stream for the chosen output with embedded cursor
      if (kde_screencast_v1_) {
        kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_output(kde_screencast_v1_, output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
        zkde_screencast_stream_unstable_v1_add_listener(kde_screencast_stream_v1_, &stream_listener, this);
      } else {
        // No screencast protocol found. Output an error based on newly initialized permission file.
        if (screencast_permission_helper_t::is_newly_initialized()) {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. "sv
                              "A new permission desktop file was automatically created but might now have been recognized yet. "sv
                              "Try restarting sunshine or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        } else {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. Check permission desktop file "sv
                              "for sunshine binary or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        }
        return -1;
      }

      if (wait_for_stream() < 0) {
        return -1;
      }

      if (stream_failed) {
        BOOST_LOG(error) << "[kwingrab] stream_output failed: "sv << stream_error_msg;
        return -1;
      }
      // Check for valid node_id and/or object serial values here, stream_ready is just an internal flag
      if (out_node_id == PW_ID_ANY && (out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(error) << "[kwingrab] timeout waiting for created event"sv;
        return -1;
      }

      if ((out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: node="sv << out_node_id;
      } else {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: objectserial="sv << out_objectserial << " (node="sv << out_node_id << ")"sv;
      }

      if (out_params->width == 0 || out_params->height == 0) {
        BOOST_LOG(error) << "[kwingrab] could not determine output dimensions"sv;
        return -1;
      }

      BOOST_LOG(info) << "[kwingrab] Screencasting output"sv
                      << " name "sv << out_params->name
                      << " position "sv << out_params->pos_x << "x"sv << out_params->pos_y
                      << " resolution "sv << out_params->width << "x"sv << out_params->height;
      return 0;
    }

    uint32_t out_node_id = PW_ID_ANY;  ///< Out node ID.
    uint64_t out_objectserial = SPA_ID_INVALID;  ///< Out objectserial.
    std::shared_ptr<output_parameter_t> out_params = nullptr;  ///< Out params.

  private:
    // Wayland objects
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct kde_output_order_v1 *kde_output_order = nullptr;
    struct zkde_screencast_unstable_v1 *kde_screencast_v1_ = nullptr;
    struct zkde_screencast_stream_unstable_v1 *kde_screencast_stream_v1_ = nullptr;
    std::map<struct wl_output *, std::shared_ptr<output_parameter_t>> outputs;
    std::vector<std::string> output_order;
    bool stream_failed = false;
    bool stream_ready = false;
    std::string stream_error_msg;

    // Misc functions
    int wait_for_stream() {
      // Dispatch until we get created/failed, with a 5s timeout
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stream_ready && !stream_failed && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(wl_display);

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(wl_display);
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining.count() <= 0) {
          break;
        }

        if (poll(&pfd, 1, remaining.count()) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] wl_display_dispatch failed"sv;
          return -1;
        }
      }
      return 0;
    }

    size_t get_order_for_output_name(const std::string_view &name) const {
      for (size_t i = 0; i < output_order.size(); i++) {
        if (output_order[i] == name) {
          return i;
        }
      }
      // If nothing matches return list size (to ensure highest order)
      return output_order.size();
    }

    // Registry listener
    static void on_registry_global(void *data, struct wl_registry *reg, const uint32_t name, const char *interface, const uint32_t version) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      auto *self = static_cast<screencast_t *>(data);
      if (!std::strcmp(interface, kde_output_order_v1_interface.name)) {
        // Bind version 1
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(1));
        self->kde_output_order = static_cast<struct kde_output_order_v1 *>(
          wl_registry_bind(reg, name, &kde_output_order_v1_interface, bind_ver)
        );
        kde_output_order_v1_add_listener(self->kde_output_order, &output_order_listener, self);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_order_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
        // Bind version 1 to 6 — We use stream_output from v1 for node_id (deprecated but good as a fall-back)
        //                       but also try to get the newer (re-use safe) pipewire objectserial from v6
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(6));
        self->kde_screencast_v1_ = static_cast<struct zkde_screencast_unstable_v1 *>(
          wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, bind_ver)
        );
        BOOST_LOG(debug) << "[kwingrab] bound zkde_screencast_unstable_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, wl_output_interface.name)) {
        // Bind version 4 - we need wl_output name for matching
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(4));
        auto *output = static_cast<struct wl_output *>(
          wl_registry_bind(reg, name, &wl_output_interface, bind_ver)
        );

        const auto [_, inserted] = self->outputs.try_emplace(output, std::make_shared<output_parameter_t>());
        if (inserted) {
          wl_output_add_listener(output, &output_listener, self);
          BOOST_LOG(debug) << "[kwingrab] bound wl_output version "sv << bind_ver << " instance: "sv << output;
        } else {
          // If we for some odd reason cannot add the output to the map clean it up and log a warning
          BOOST_LOG(warning) << "[kwingrab] Ignoring output "sv << output << " because map emplace failed."sv;
          wl_output_destroy(output);
        }
      }
    }

    static void on_registry_global_remove(void *data [[maybe_unused]], struct wl_registry *reg [[maybe_unused]], uint32_t name [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      // We don't handle output hot-unplug during init
    }

    static constexpr struct wl_registry_listener registry_listener = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    // wl_output listener (for mode/dimensions/name)
    static void on_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw [[maybe_unused]], int32_t ph [[maybe_unused]], int32_t subpixel [[maybe_unused]], const char *make [[maybe_unused]], const char *model [[maybe_unused]], int32_t transform [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->pos_x = x;
      output_parameter->pos_y = y;
    }

    static void on_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
        return;
      }
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->width = width;
      output_parameter->height = height;
    }

    static void on_output_done(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      // Currently unused
    }

    static void on_output_scale(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], int32_t factor [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      // Currently unused
    }

    static void on_output_name(void *data, struct wl_output *output, const char *name) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      const auto *self = static_cast<screencast_t *>(data);
      self->outputs.at(output)->name = name;
    }

    static void on_output_description(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], const char *description [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      // Currently unused
    }

    static constexpr struct wl_output_listener output_listener = {
      .geometry = on_output_geometry,
      .mode = on_output_mode,
      .done = on_output_done,
      .scale = on_output_scale,
      .name = on_output_name,
      .description = on_output_description,
    };

    // Output order listener
    static void on_output_order_output(void *data, struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]], const char *output_name) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      auto *self = static_cast<screencast_t *>(data);
      self->output_order.emplace_back(output_name);
    }

    static void on_output_order_done(void *data [[maybe_unused]], struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      // Currently unused
    }

    static constexpr kde_output_order_v1_listener output_order_listener = {
      .output = on_output_order_output,
      .done = on_output_order_done,
    };

    // ScreenCast v1 stream listener
    static void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]]) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      auto *self = static_cast<screencast_t *>(data);
      BOOST_LOG(warning) << "[kwingrab] stream closed by server"sv;
      self->stream_failed = false;
      self->stream_ready = false;
      self->stream_error_msg = "stream closed by server";
    }

    static void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const uint32_t node) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      auto *self = static_cast<screencast_t *>(data);
      self->out_node_id = node;
      self->stream_failed = false;
      self->stream_ready = true;
      BOOST_LOG(debug) << "[kwingrab] created event, node_id="sv << node;
    }

    static void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const char *err_msg) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      auto *self = static_cast<screencast_t *>(data);
      self->stream_failed = true;
      self->stream_ready = false;
      self->stream_error_msg = err_msg ? err_msg : "unknown error";
      BOOST_LOG(error) << "[kwingrab] failed event: "sv << self->stream_error_msg;
    }

    static void on_stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], uint32_t object_serial_hi, uint32_t object_serial_low) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
      auto *self = static_cast<screencast_t *>(data);
      self->out_objectserial = static_cast<uint64_t>(object_serial_hi) << 32 | object_serial_low;
      // serial event always preceded the created event with the node id, so we only set stream_ready in created for v1
      BOOST_LOG(debug) << "[kwingrab] serial event, objectserial="sv << self->out_objectserial;
    }

    static constexpr struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
      .closed = on_stream_closed,
      .created = on_stream_created,
      .failed = on_stream_failed,
      .serial = on_stream_serial,
    };
  };

  /**
   * Display backend
   *
   * Orchestrates screencast_t and implements pipewire_display_t
   */
  class kwin_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      screencast = std::make_unique<screencast_t>();
      if (screencast->init(true) < 0) {
        return -1;
      }
      if (screencast->start(display_name) < 0) {
        return -1;
      }
      if (screencast->out_params) {
        // Return values for pipewire init
        out_pipewire_fd = -1;  // KWin screencast capture runs on the local pipewire core
        out_pipewire_node = screencast->out_node_id;
        out_pipewire_objectserial = screencast->out_objectserial;
        // Set/update basic stream parameters on display_t
        this->output_name = screencast->out_params->name;
        this->offset_x = screencast->out_params->pos_x;
        this->offset_y = screencast->out_params->pos_y;
        this->width = screencast->out_params->width;
        this->height = screencast->out_params->height;
        this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
        this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.
        return 0;
      }
      return -1;
    }

    std::unique_ptr<screencast_t> screencast;  ///< Screencast.
  };
}  // namespace kwin

// Public API for misc.cpp
namespace platf {
  /**
   * @brief Create a KWin screencast display backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return KWin/PipeWire display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> kwin_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[kwingrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<kwin::kwin_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate KWin screencast display names.
   *
   * @return KWin display names, or an empty list when KWin capture is unavailable.
   */
  std::vector<std::string> kwin_display_names() {
    const auto screencast = std::make_unique<kwin::screencast_t>();
    if (screencast->init() < 0) {
      return {};
    }
    return screencast->get_output_names();
  }

  /**
   * @brief Check whether KWin screencast capture is available.
   *
   * @return True when KWin capture support is available.
   */
  bool kwin_available() {
    // Init screencast without permission setup (to not cause unneeded logs / temporary desktop files) and check KWin availability
    if (const auto screencast = std::make_unique<kwin::screencast_t>(); screencast->init(false) < 0 || !screencast->kwin_available()) {
      return false;
    }
    return true;
  }
}  // namespace platf
