/**
 * @file src/platform/linux/portalgrab.cpp
 * @brief Definitions for XDG portal grab.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <cstdlib>

// lib includes
#include <lizardbyte/common/env.h>

// local includes
#include "pipewire.cpp"
#include "src/globals.h"

namespace {
  // Portal configuration constants
  constexpr uint32_t SOURCE_TYPE_MONITOR = 1;
  constexpr uint32_t CURSOR_MODE_EMBEDDED = 2;

  constexpr uint32_t PERSIST_FORGET = 0;
  constexpr uint32_t PERSIST_WHILE_RUNNING = 1;
  constexpr uint32_t PERSIST_UNTIL_REVOKED = 2;

  constexpr uint32_t TYPE_KEYBOARD = 1;
  constexpr uint32_t TYPE_POINTER = 2;
  constexpr uint32_t TYPE_TOUCHSCREEN = 4;

  // Portal D-Bus interface names and paths
  constexpr const char *PORTAL_NAME = "org.freedesktop.portal.Desktop";
  constexpr const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
  constexpr const char *REMOTE_DESKTOP_IFACE = "org.freedesktop.portal.RemoteDesktop";
  constexpr const char *SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
  constexpr const char *REQUEST_IFACE = "org.freedesktop.portal.Request";

  constexpr const char REQUEST_PREFIX[] = "/org/freedesktop/portal/desktop/request/";
  constexpr const char SESSION_PREFIX[] = "/org/freedesktop/portal/desktop/session/";
}  // namespace

using namespace std::literals;

namespace portal {
  // Forward declarations
  class runtime_t;

  /**
   * @brief Persistent portal restore token used to reuse screencast permission.
   */
  class restore_token_t {
  public:
    /**
     * @brief Return the currently wrapped value or handle.
     *
     * @return Underlying native handle or object pointer.
     */
    static std::string get() {
      return *token_;
    }

    /**
     * @brief Store the new value and mark it dirty for persistence.
     *
     * @param value Portal restore token received from xdg-desktop-portal.
     */
    static void set(std::string_view value) {
      *token_ = value;
    }

    /**
     * @brief Return whether the persisted value is empty.
     *
     * @return True when no portal display id has been persisted.
     */
    static bool empty() {
      return token_->empty();
    }

    /**
     * @brief Load persisted state from its backing store.
     */
    static void load() {
      std::ifstream file(get_file_path());
      if (file.is_open()) {
        std::getline(file, *token_);
        if (!token_->empty()) {
          BOOST_LOG(info) << "[portalgrab] Loaded portal restore token from disk"sv;
        }
      }
    }

    /**
     * @brief Check if a Portal restore token exists on disk without inspecting its contents.
     *
     * @return True if file exists on disk.
     */
    static bool exists() {
      std::error_code ec;
      return std::filesystem::exists(get_file_path(), ec);
    }

    /**
     * @brief Clear a restore token if it already exists on disk.
     */
    static void clear() {
      std::error_code ec;
      token_->clear();
      std::filesystem::remove(get_file_path(), ec);
    }

    /**
     * @brief Save current state to its backing store.
     */
    static void save() {
      if (token_->empty()) {
        return;
      }
      std::ofstream file(get_file_path());
      if (file.is_open()) {
        file << *token_;
        BOOST_LOG(info) << "[portalgrab] Saved portal restore token to disk"sv;
      } else {
        BOOST_LOG(warning) << "[portalgrab] Failed to save portal restore token"sv;
      }
    }

    /**
     * @brief Get file path of the XDG Portal restore token.
     *
     * The token filename is suffixed with the XDG session desktop when set in the environment.
     *
     * @param directory Directory containing saved tokens; defaults to the application data directory.
     * @param desktop Session desktop; defaults to the XDG_SESSION_DESKTOP environment variable.
     * @return File path.
     */
    static std::filesystem::path get_file_path(
      const std::filesystem::path &directory = platf::appdata(),
      const std::string &desktop = lizardbyte::common::get_env("XDG_SESSION_DESKTOP")
    ) {
      const std::filesystem::path legacy_path = directory / "portal_token";

      std::string suffix(desktop);
      boost::algorithm::to_lower(suffix);

      // Restrict the suffix to prevent path traversal and other invalid filename characters.
      if (const bool is_safe = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
          });
          !is_safe) {
        BOOST_LOG(warning) << "[portalgrab] XDG session desktop type cannot be determined. Using .unknown suffix for Portal restore token."sv;
        suffix = "unknown";
      }

      const std::filesystem::path suffixed_path = directory / ("portal_token." + suffix);

      // One-time migration of legacy portal_token.
      std::error_code ec;
      if (!std::filesystem::exists(suffixed_path, ec) && !ec) {
        const bool legacy_exists = std::filesystem::exists(legacy_path, ec);

        if (!ec && legacy_exists) {
          // Attempt one-time adoption of a pre-upgrade token. Best-effort guess that it belongs
          // to this session type; if wrong, we're no worse off than not migrating.
          std::filesystem::rename(legacy_path, suffixed_path, ec);
          if (ec) {
            BOOST_LOG(error) << "[portalgrab] Portal restore token migration failed: "sv << ec.message();
          } else {
            BOOST_LOG(info) << "[portalgrab] Portal restore token migrated to portal_token."sv << suffix;
          }
        }
      }

      return suffixed_path;
    }

  private:
    static inline const std::unique_ptr<std::string> token_ = std::make_unique<std::string>();
  };

  /**
   * @brief Clear a restore token if it already exists on disk.
   */
  void clear_saved_token() {
    restore_token_t::clear();
  }

  /**
   * @brief Check if a Portal restore token exists on disk without inspecting its contents.
   *
   * @return True if a saved token was found.
   */
  bool has_saved_token() {
    return restore_token_t::exists();
  }

  /**
   * @brief Check if the Portal service responds to a DBus Ping within 2 seconds.
   *
   * @return True if the Portal is reachable.
   */
  bool is_portal_service_reachable() {
    g_autoptr(GError) g_error = nullptr;
    g_autofree const gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &g_error);
    if (!address) {
      return false;
    }

    g_autoptr(GError) ping_error = nullptr;
    g_autoptr(GDBusConnection) conn = g_dbus_connection_new_for_address_sync(
      address,
      GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr,
      nullptr,
      &ping_error
    );
    if (!conn) {
      return false;
    }

    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
      conn,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.DBus.Peer",
      "Ping",
      nullptr,
      nullptr,
      G_DBUS_CALL_FLAGS_NONE,
      2000,
      nullptr,
      &ping_error
    );
    return reply != nullptr;
  }

  /**
   * @brief DBus response loop and response variant for portal calls.
   */
  struct dbus_response_t {
    GMainLoop *loop;  ///< GLib main loop waiting for a portal response signal.
    GVariant *response;  ///< DBus response payload returned by the portal.
    guint subscription_id;  ///< Subscription ID.
    std::string request_path;  ///< For Request.Close() on cancellation.
    GDBusConnection *conn;  ///< Borrowed — owned by the calling dbus_t/portal_t.

    dbus_response_t() = default;
    dbus_response_t(const dbus_response_t &) = delete;
    dbus_response_t &operator=(const dbus_response_t &) = delete;

    /**
     * @brief Stop listening if the request is abandoned before its response is awaited.
     */
    ~dbus_response_t() {
      if (subscription_id != 0) {
        g_dbus_connection_signal_unsubscribe(conn, subscription_id);
      }
    }
  };

  /**
   * @brief Get path of the XDG Portal restore token.
   *
   * @return Path of token that may include the session desktop as a suffix (e.g. .gnome, .kde).
   */
  std::filesystem::path get_saved_token_path() {
    return restore_token_t::get_file_path();
  }

#ifdef SUNSHINE_TESTS
  std::filesystem::path get_saved_token_path_for_testing(const std::filesystem::path &directory, const std::string &desktop) {
    return restore_token_t::get_file_path(directory, desktop);
  }
#endif

  /**
   * @brief PipeWire stream node and negotiated capture size.
   */
  struct pipewire_streaminfo_t {
    uint32_t pipewire_node = PW_ID_ANY;  ///< PipeWire node ID selected by the portal.
    uint64_t pipewire_object_serial = SPA_ID_INVALID;  ///< PipeWire object serial selected by the portal.
    int width = 0;  ///< Stream width in pixels.
    int height = 0;  ///< Stream height in pixels.
    int pos_x = 0;  ///< Output X position reported by the portal.
    int pos_y = 0;  ///< Output Y position reported by the portal.
    std::string monitor_name;  ///< Monitor name.

    /**
     * @brief Convert to display name.
     *
     * @return Value converted to display name.
     */
    std::string to_display_name() {
      if (!monitor_name.empty()) {
        return monitor_name;
      }
      return std::format("position-{}x{}-resolution-{}x{}", pos_x, pos_y, width, height);
    }

    /**
     * @brief Check whether a portal stream matches a requested display name.
     *
     * @param display_name Display name.
     * @return True when the portal display id matches the requested display name.
     */
    bool match_display_name(const std::string_view &display_name) {
      // Check the given non-empty display name matches the display name for this struct
      return !display_name.empty() && display_name == to_display_name();
    }
  };

  /**
   * @brief DBus connection and portal request helpers for screencast setup.
   */
  class dbus_t {
  public:
    guint dbus_timeout = 10;  ///< Timeout in seconds for DBus calls.

    dbus_t &operator=(dbus_t &&) = delete;  // Do not allow to copying

    ~dbus_t() noexcept {
      try {
        if (conn && !session_handle.empty()) {
          g_autoptr(GError) err = nullptr;
          // This is a blocking C call; it won't throw, but we wrap for safety
          g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.portal.Desktop",
            session_handle.c_str(),
            "org.freedesktop.portal.Session",
            "Close",
            nullptr,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            dbus_timeout * 1000,
            nullptr,
            &err
          );

          if (err) {
            BOOST_LOG(warning) << "[portalgrab] Failed to explicitly close portal session: "sv << err->message;
          } else {
            BOOST_LOG(debug) << "[portalgrab] Explicitly closed portal session: "sv << session_handle;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "[portalgrab] Standard exception caught in ~dbus_t: "sv << e.what();
      } catch (...) {
        BOOST_LOG(error) << "[portalgrab] Unknown exception caught in ~dbus_t"sv;
      }

      if (pipewire_fd >= 0) {
        close(pipewire_fd);
      }
      if (screencast_proxy) {
        g_clear_object(&screencast_proxy);
      }
      if (remote_desktop_proxy) {
        g_clear_object(&remote_desktop_proxy);
      }
      if (conn) {
        g_clear_object(&conn);
      }
    }

    /**
     * @brief Open DBus and prepare portal screencast request handling.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      restore_token_t::load();

      g_autoptr(GError) g_error = nullptr;
      g_autofree gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &g_error);
      if (!address) {
        return -1;
      }

      conn = g_dbus_connection_new_for_address_sync(
        address,
        GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr,
        nullptr,
        &g_error
      );
      if (!conn) {
        return -1;
      }

      remote_desktop_proxy = g_dbus_proxy_new_sync(
        conn,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        PORTAL_NAME,
        PORTAL_PATH,
        REMOTE_DESKTOP_IFACE,
        nullptr,
        &g_error
      );
      if (!remote_desktop_proxy) {
        return -1;
      }

      screencast_proxy = g_dbus_proxy_new_sync(
        conn,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        PORTAL_NAME,
        PORTAL_PATH,
        SCREENCAST_IFACE,
        nullptr,
        &g_error
      );
      if (!screencast_proxy) {
        return -1;
      }

      return 0;
    }

    /**
     * @brief Connect to xdg-desktop-portal and restore or create a screencast session.
     *
     * @param allow_start_timeout True if "Start" DBus call is allowed to time out.
     * @return 0 when a portal session is ready; nonzero when D-Bus or portal setup fails.
     */
    int connect_to_portal(bool allow_start_timeout) {
      g_autoptr(GMainContext) context = g_main_context_new();
      g_autoptr(GMainLoop) loop = g_main_loop_new(context, false);
      g_autofree gchar *session_path = nullptr;
      g_autofree gchar *session_token = nullptr;
      create_session_path(conn, nullptr, &session_token);

      // Try combined RemoteDesktop + ScreenCast session first
      bool use_screencast_only = !try_remote_desktop_session(loop, &session_path, session_token);

      // Fall back to ScreenCast-only if RemoteDesktop failed
      if (use_screencast_only && try_screencast_only_session(loop, &session_path) < 0) {
        return -1;
      }

      if (start_portal_session(loop, session_path, pipewire_streams, use_screencast_only, allow_start_timeout) < 0) {
        return -1;
      }

      if (open_pipewire_remote(session_path, pipewire_fd) < 0) {
        return -1;
      }

      return 0;
    }

    // Try to create a combined RemoteDesktop + ScreenCast session
    // Returns true on success, false if should fall back to ScreenCast-only
    /**
     * @brief Try to create a RemoteDesktop portal session.
     *
     * @param loop GLib main loop associated with the portal request.
     * @param session_path Session path.
     * @param session_token Session token.
     * @return True when the portal request or state check succeeds.
     */
    bool try_remote_desktop_session(GMainLoop *loop, gchar **session_path, const gchar *session_token) {
      if (create_portal_session(loop, session_path, session_token, false) < 0) {
        return false;
      }

      if (select_remote_desktop_devices(loop, *session_path) < 0) {
        BOOST_LOG(warning) << "[portalgrab] RemoteDesktop.SelectDevices failed, falling back to ScreenCast-only mode"sv;
        g_free(*session_path);
        *session_path = nullptr;
        return false;
      }

      if (select_screencast_sources(loop, *session_path, false) < 0) {
        BOOST_LOG(warning) << "[portalgrab] ScreenCast.SelectSources failed with RemoteDesktop session, trying ScreenCast-only mode"sv;
        g_free(*session_path);
        *session_path = nullptr;
        return false;
      }

      return true;
    }

    // Create a ScreenCast-only session
    /**
     * @brief Create a screencast-only portal session without remote-desktop control.
     *
     * @param loop GLib main loop associated with the portal request.
     * @param session_path Session path.
     * @return 0 when the portal returns a session path; nonzero on request failure.
     */
    int try_screencast_only_session(GMainLoop *loop, gchar **session_path) {
      g_autofree gchar *new_session_token = nullptr;
      create_session_path(conn, nullptr, &new_session_token);
      if (create_portal_session(loop, session_path, new_session_token, true) < 0) {
        return -1;
      }
      if (select_screencast_sources(loop, *session_path, true) < 0) {
        g_free(*session_path);
        *session_path = nullptr;
        return -1;
      }
      return 0;
    }

    /**
     * @brief Check whether session closed.
     *
     * @return True when the portal session has been closed.
     */
    bool is_session_closed() const {
      if (conn && !session_handle.empty()) {
        // Try to retrieve property org.freedesktop.portal.Session::version
        g_autoptr(GError) err = nullptr;
        g_dbus_connection_call_sync(
          conn,
          "org.freedesktop.portal.Desktop",
          session_handle.c_str(),
          "org.freedesktop.DBus.Properties",
          "Get",
          g_variant_new("(ss)", "org.freedesktop.portal.Session", "version"),
          G_VARIANT_TYPE("(v)"),
          G_DBUS_CALL_FLAGS_NONE,
          dbus_timeout * 1000,
          nullptr,
          &err
        );
        // If we cannot get the property then the session portal was closed.
        if (err) {
          BOOST_LOG(debug) << "[portalgrab] Session closed as check failed: "sv << err->message;
          return true;
        }
      }
      // The session is not closed (or might not have been opened yet).
      return false;
    }

    std::vector<pipewire_streaminfo_t> pipewire_streams;  ///< Pipewire streams.
    int pipewire_fd;  ///< Pipewire fd.

  private:
    GDBusConnection *conn;
    GDBusProxy *screencast_proxy;
    GDBusProxy *remote_desktop_proxy;
    std::string session_handle;

    int create_portal_session(GMainLoop *loop, gchar **session_path_out, const gchar *session_token, bool use_screencast) {
      GDBusProxy *proxy = use_screencast ? screencast_proxy : remote_desktop_proxy;
      const char *session_type = use_screencast ? "ScreenCast" : "RemoteDesktop";

      dbus_response_t response {};
      g_autofree gchar *request_token = nullptr;
      dbus_response_prepare(&response, loop, conn, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(a{sv})"));
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(proxy, "CreateSession", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, dbus_timeout * 1000, nullptr, &err);

      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not create "sv << session_type << " session: "sv << err->message;
        return -1;
      }

      dbus_response_follow_handle(&response, reply);

      g_autoptr(GVariant) create_response = dbus_response_wait(&response, dbus_timeout);

      if (!create_response) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) results = nullptr;
      g_variant_get(create_response, "(u@a{sv})", &response_code, &results);

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " CreateSession response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession failed with response code: "sv << response_code;
        return -1;
      }

      g_autoptr(GVariant) session_handle_v = g_variant_lookup_value(results, "session_handle", nullptr);
      if (!session_handle_v) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " CreateSession: session_handle not found in response"sv;
        return -1;
      }

      if (g_variant_is_of_type(session_handle_v, G_VARIANT_TYPE_VARIANT)) {
        g_autoptr(GVariant) inner = g_variant_get_variant(session_handle_v);
        *session_path_out = g_strdup(g_variant_get_string(inner, nullptr));
      } else {
        *session_path_out = g_strdup(g_variant_get_string(session_handle_v, nullptr));
      }

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " CreateSession: got session handle: "sv << *session_path_out;
      // Save it for the destructor to use during cleanup
      this->session_handle = *session_path_out;
      return 0;
    }

    int select_remote_desktop_devices(GMainLoop *loop, const gchar *session_path) {
      dbus_response_t response {};
      g_autofree gchar *request_token = nullptr;
      dbus_response_prepare(&response, loop, conn, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(TYPE_KEYBOARD | TYPE_POINTER | TYPE_TOUCHSCREEN));
      g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
      if (!restore_token_t::empty()) {
        g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(restore_token_t::get().c_str()));
      }
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(remote_desktop_proxy, "SelectDevices", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, dbus_timeout * 1000, nullptr, &err);

      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not select devices: "sv << err->message;
        return -1;
      }

      dbus_response_follow_handle(&response, reply);

      g_autoptr(GVariant) devices_response = dbus_response_wait(&response, dbus_timeout);

      if (!devices_response) {
        BOOST_LOG(error) << "[portalgrab] SelectDevices: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(devices_response, "(u@a{sv})", &response_code, nullptr);
      BOOST_LOG(debug) << "[portalgrab] SelectDevices response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] SelectDevices failed with response code: "sv << response_code;
        return -1;
      }

      return 0;
    }

    int select_screencast_sources(GMainLoop *loop, const gchar *session_path, bool persist) {
      dbus_response_t response {};
      g_autofree gchar *request_token = nullptr;
      dbus_response_prepare(&response, loop, conn, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(SOURCE_TYPE_MONITOR));
      g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(CURSOR_MODE_EMBEDDED));
      g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(TRUE));
      if (persist) {
        g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
        if (!restore_token_t::empty()) {
          g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(restore_token_t::get().c_str()));
        }
      }
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(screencast_proxy, "SelectSources", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, dbus_timeout * 1000, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not select sources: "sv << err->message;
        return -1;
      }

      dbus_response_follow_handle(&response, reply);

      g_autoptr(GVariant) sources_response = dbus_response_wait(&response, dbus_timeout);

      if (!sources_response) {
        BOOST_LOG(error) << "[portalgrab] SelectSources: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(sources_response, "(u@a{sv})", &response_code, nullptr);
      BOOST_LOG(debug) << "[portalgrab] SelectSources response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] SelectSources failed with response code: "sv << response_code;
        return -1;
      }

      return 0;
    }

    int start_portal_session(GMainLoop *loop, const gchar *session_path, std::vector<pipewire_streaminfo_t> &out_pipewire_streams, bool use_screencast, bool allow_start_timeout) {
      GDBusProxy *proxy = use_screencast ? screencast_proxy : remote_desktop_proxy;
      const char *session_type = use_screencast ? "ScreenCast" : "RemoteDesktop";

      dbus_response_t response {};
      g_autofree gchar *request_token = nullptr;
      dbus_response_prepare(&response, loop, conn, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(osa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_add(&builder, "s", "");  // parent_window
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(proxy, "Start", g_variant_builder_end(&builder), G_DBUS_CALL_FLAGS_NONE, dbus_timeout * 1000, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not start "sv << session_type << " session: "sv << err->message;
        return -1;
      }

      dbus_response_follow_handle(&response, reply);

      g_autoptr(GVariant) start_response = dbus_response_wait(&response, (allow_start_timeout ? dbus_timeout : 0));

      if (!start_response) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) dict = nullptr;
      g_autoptr(GVariant) streams = nullptr;
      g_variant_get(start_response, "(u@a{sv})", &response_code, &dict);

      BOOST_LOG(debug) << "[portalgrab] " << session_type << " Start response_code: "sv << response_code;

      if (response_code != 0) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start failed with response code: "sv << response_code;
        return -1;
      }

      streams = g_variant_lookup_value(dict, "streams", G_VARIANT_TYPE("a(ua{sv})"));
      if (!streams) {
        BOOST_LOG(error) << "[portalgrab] " << session_type << " Start: no streams in response"sv;
        return -1;
      }

      if (const gchar *new_token = nullptr; g_variant_lookup(dict, "restore_token", "s", &new_token) && new_token && new_token[0] != '\0' && restore_token_t::get() != new_token) {
        restore_token_t::set(new_token);
        restore_token_t::save();
      }

      GVariantIter iter;
      const auto wl_monitors = wl::monitors();
      uint32_t out_pipewire_node;
      g_autoptr(GVariant) value = nullptr;
      g_variant_iter_init(&iter, streams);
      while (g_variant_iter_next(&iter, "(u@a{sv})", &out_pipewire_node, &value)) {
        int out_width;
        int out_height;
        bool result = g_variant_lookup(value, "size", "(ii)", &out_width, &out_height, nullptr);
        if (!result) {
          BOOST_LOG(warning) << "[portalgrab] Ignoring stream without proper resolution on pipewire node "sv << out_pipewire_node;
          continue;
        }

        int out_pos_x;
        int out_pos_y;
        result = g_variant_lookup(value, "position", "(ii)", &out_pos_x, &out_pos_y, nullptr);
        if (!result) {
          BOOST_LOG(warning) << "[portalgrab] Falling back to position 0x0 for stream with resolution "sv << out_width << "x"sv << out_height << "on pipewire node "sv << out_pipewire_node;
          out_pos_x = 0;
          out_pos_y = 0;
        }

        uint64_t out_pipewire_object_serial;
        result = g_variant_lookup(value, "pipewire-serial", "t", &out_pipewire_object_serial);
        if (!result) {
          // If pipewire-serial was not present explicitly set to invalid value.
          out_pipewire_object_serial = SPA_ID_INVALID;
        }

        auto stream = pipewire_streaminfo_t {
          .pipewire_node = out_pipewire_node,
          .pipewire_object_serial = out_pipewire_object_serial,
          .width = out_width,
          .height = out_height,
          .pos_x = out_pos_x,
          .pos_y = out_pos_y,
        };

        // Try to match the stream to a monitor_name by position/resolution and update stream info
        for (const auto &monitor : wl_monitors) {
          if (monitor->viewport.offset_x == out_pos_x && monitor->viewport.offset_y == out_pos_y && monitor->viewport.logical_width == out_width && monitor->viewport.logical_height == out_height) {
            stream.monitor_name = monitor->name;
            break;
          }
        }

        out_pipewire_streams.emplace_back(stream);
      }

      // The portal call returns the streams sorted by out_pipewire_node which can shuffle displays around, so
      // we have to sort pipewire streams by position here to be consistent
      std::ranges::sort(out_pipewire_streams, [](const auto &a, const auto &b) {
        return a.pos_x < b.pos_x || a.pos_y < b.pos_y;
      });

      return 0;
    }

    int open_pipewire_remote(const gchar *session_path, int &fd) {
      g_autoptr(GUnixFDList) fd_list = nullptr;
      g_autoptr(GVariant) msg = g_variant_ref_sink(g_variant_new("(oa{sv})", session_path, nullptr));

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_with_unix_fd_list_sync(screencast_proxy, "OpenPipeWireRemote", msg, G_DBUS_CALL_FLAGS_NONE, dbus_timeout * 1000, nullptr, &fd_list, nullptr, &err);
      if (err) {
        BOOST_LOG(error) << "[portalgrab] Could not open pipewire remote: "sv << err->message;
        return -1;
      }

      int fd_handle;
      g_variant_get(reply, "(h)", &fd_handle);
      fd = g_unix_fd_list_get(fd_list, fd_handle, nullptr);
      return 0;
    }

    static void on_response_received_cb([[maybe_unused]] GDBusConnection *connection, [[maybe_unused]] const gchar *sender_name, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, [[maybe_unused]] const gchar *signal_name, GVariant *parameters, gpointer user_data) {
      auto *response = static_cast<dbus_response_t *>(user_data);
      response->response = g_variant_ref_sink(parameters);
      g_main_loop_quit(response->loop);
    }

    static gchar *get_sender_string(GDBusConnection *conn) {
      gchar *sender = g_strdup(g_dbus_connection_get_unique_name(conn) + 1);
      gchar *dot;
      while ((dot = strstr(sender, ".")) != nullptr) {
        *dot = '_';
      }
      return sender;
    }

    static void create_request_path(GDBusConnection *conn, gchar **out_path, gchar **out_token) {
      static uint32_t request_count = 0;

      request_count++;

      if (out_token) {
        *out_token = g_strdup_printf("Sunshine%u", request_count);
      }
      if (out_path) {
        g_autofree gchar *sender = get_sender_string(conn);
        *out_path = g_strdup(std::format("{}{}{}{}", REQUEST_PREFIX, sender, "/Sunshine", request_count).c_str());
      }
    }

    static void create_session_path(GDBusConnection *conn, gchar **out_path, gchar **out_token) {
      static uint32_t session_count = 0;

      session_count++;

      if (out_token) {
        *out_token = g_strdup_printf("Sunshine%u", session_count);
      }

      if (out_path) {
        g_autofree gchar *sender = get_sender_string(conn);
        *out_path = g_strdup(std::format("{}{}{}{}", SESSION_PREFIX, sender, "/Sunshine", session_count).c_str());
      }
    }

    /**
     * @brief Asynchronously close a pending Portal request.
     */
    static void close_request_async(GDBusConnection *conn, const std::string &request_path) {
      g_dbus_connection_call(
        conn,
        PORTAL_NAME,
        request_path.c_str(),
        REQUEST_IFACE,
        "Close",
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr,
        nullptr
      );
    }

    static void dbus_response_init(struct dbus_response_t *response, GMainLoop *loop, GDBusConnection *conn, const char *request_path) {
      response->loop = loop;
      response->conn = conn;
      response->request_path = request_path;

      GMainContext *context = g_main_loop_get_context(loop);
      g_main_context_push_thread_default(context);

      response->subscription_id = g_dbus_connection_signal_subscribe(
        conn,
        PORTAL_NAME,
        REQUEST_IFACE,
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_response_received_cb,
        response,
        nullptr
      );

      g_main_context_pop_thread_default(context);
    }

    /**
     * @brief Subscribe to the Response signal of a portal request before making the request.
     *
     * The portal can emit Response within a millisecond of replying to the method call, so subscribing only after the
     * reply may miss it and wait forever. The request path is predictable from handle_token, as documented for
     * org.freedesktop.portal.Request.
     *
     * @param response DBus response to subscribe.
     * @param loop GLib main loop that will wait for the response.
     * @param conn DBus connection used for the request.
     * @param out_token Receives the handle_token to pass in the request options.
     */
    static void dbus_response_prepare(dbus_response_t *response, GMainLoop *loop, GDBusConnection *conn, gchar **out_token) {
      g_autofree gchar *request_path = nullptr;
      create_request_path(conn, &request_path, out_token);
      dbus_response_init(response, loop, conn, request_path);
    }

    /**
     * @brief Move the subscription to the request handle returned by the portal if it differs from the predicted one.
     *
     * xdg-desktop-portal versions before 0.9 do not derive the handle from handle_token. On those versions a Response
     * sent before this re-subscription is still missed, as it was before handle_token was used.
     *
     * @param response DBus response subscribed by dbus_response_prepare().
     * @param reply Reply of the portal method call that created the request.
     */
    static void dbus_response_follow_handle(dbus_response_t *response, GVariant *reply) {
      const gchar *request_path = nullptr;
      g_variant_get(reply, "(&o)", &request_path);
      if (response->request_path != request_path) {
        g_dbus_connection_signal_unsubscribe(response->conn, response->subscription_id);
        dbus_response_init(response, response->loop, response->conn, request_path);
      }
    }

    /**
     * @brief Data for loop and shutdown_event mail used by check_shutdown_cb().
     */
    struct loop_context_t {
      GMainLoop *loop;
      std::shared_ptr<safe::event_t<bool>> shutdown_event;
    };

    /**
     * @brief Check for Sunshine shutdown and quit the Portal response loop if requested.
     *
     * @result True (continue) if shutdown event is not in progress.
     */
    static gboolean check_shutdown_cb(gpointer user_data) {
      if (auto *ctx = static_cast<loop_context_t *>(user_data); ctx->shutdown_event && ctx->shutdown_event->peek()) {
        g_main_loop_quit(ctx->loop);
        return G_SOURCE_REMOVE;
      }
      return G_SOURCE_CONTINUE;
    }

    /**
     * @brief Check for DBus response with optional timeout guard.
     *
     * @param response DBus response.
     * @param timeout_seconds Timeout in seconds before quitting loop.
     * @return Variant containing the requested data.
     */
    static GVariant *dbus_response_wait(dbus_response_t *response, guint timeout_seconds = 0) {
      GSource *timeout_source = nullptr;

      if (timeout_seconds > 0) {
        timeout_source = g_timeout_source_new(timeout_seconds * 1000);
        g_source_set_callback(
          timeout_source,
          [](gpointer user_data) {
            g_main_loop_quit(static_cast<GMainLoop *>(user_data));
            return G_SOURCE_REMOVE;
          },
          response->loop,
          nullptr
        );
        g_source_attach(timeout_source, g_main_loop_get_context(response->loop));
      }

      constexpr guint shutdown_poll_interval_ms = 1000;
      auto shutdown_event = mail::man ? mail::man->event<bool>(mail::shutdown) : nullptr;
      loop_context_t ctx {response->loop, shutdown_event};

      GSource *shutdown_source = g_timeout_source_new(shutdown_poll_interval_ms);
      g_source_set_callback(shutdown_source, check_shutdown_cb, &ctx, nullptr);
      g_source_attach(shutdown_source, g_main_loop_get_context(response->loop));

      g_main_loop_run(response->loop);

      if (response->subscription_id != 0) {
        g_dbus_connection_signal_unsubscribe(
          response->conn,
          response->subscription_id
        );
      }
      response->subscription_id = 0;

      g_source_destroy(shutdown_source);
      g_source_unref(shutdown_source);

      if (timeout_source) {
        g_source_destroy(timeout_source);
        g_source_unref(timeout_source);
      }

      if (response->response) {
        return response->response;
      }

      BOOST_LOG(info) << "[portalgrab] Portal request cancelled, timed out, or shutdown requested"sv;
      close_request_async(response->conn, response->request_path);
      return nullptr;
    }
  };

  /**
   * @brief Portal screencast backend that negotiates PipeWire streams over DBus.
   */
  class portal_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_object_serial [[maybe_unused]]) override {
      // Connect DBus portal session
      if (dbus.init() < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to connect to dbus. portal_t setup failed.";
        return -1;
      }
      if (dbus.connect_to_portal(false) < 0) {
        BOOST_LOG(error) << "[portalgrab] Failed to connect to portal. portal_t setup failed.";
        return -1;
      }

      // Match display_name to a stream from the pipewire_streams vector
      bool use_fallback = true;
      pipewire_streaminfo_t stream;
      auto streams = dbus.pipewire_streams;
      if (streams.empty()) {
        BOOST_LOG(error) << "[portalgrab] No streams found on portal. portal_t setup failed.";
        return -1;
      }
      for (auto &stream_ : streams) {
        if (stream_.match_display_name(display_name)) {
          stream = stream_;
          use_fallback = false;
          break;
        }
      }
      // Fall back to first stream if we cannot match the given display_name to a stream in currently available streams.
      if (use_fallback) {
        BOOST_LOG(info) << "[portalgrab] Using first available stream as no matching stream was found for: '"sv << display_name << "'";
        stream = dbus.pipewire_streams.at(0);
      }

      // Restore global maxframerate negotiation state
      pipewire.set_negotiate_maxframerate(negotiate_maxframerate.load());

      // Return values for pipewire init
      out_pipewire_fd = dbus.pipewire_fd;
      out_pipewire_node = stream.pipewire_node;
      out_pipewire_object_serial = stream.pipewire_object_serial;
      // Set/update basic stream parameters on display_t
      this->output_name = stream.monitor_name;
      this->offset_x = stream.pos_x;
      this->offset_y = stream.pos_y;
      this->width = stream.width;
      this->height = stream.height;
      this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
      this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.
      // Flag successful setup
      return 0;
    }

    /**
     * @brief Handle portal closure or retry failed startup negotiation without maxFramerate.
     *
     * @param out_status Capture status selected when portal-specific handling is needed.
     * @return True when portal-specific handling selected a capture status; false to use the default stream reset.
     */
    bool check_stream_dead(platf::capture_e &out_status) override {
      // If the pipewire stream stopped due to closed portal session stop the capture with an error
      if (dbus.is_session_closed()) {
        BOOST_LOG(warning) << "[portalgrab] PipeWire stream stopped by closed portal session."sv;
        pipewire.frame_cv().notify_all();
        out_status = platf::capture_e::capture_error;
        return true;  // Stop capture with error (due to out_status)
      }
      // Disable maxframerate negotiation if the stream died without having ever started (e.g. GNOME mutter does not support it)
      if (!shared_state->has_streamed.load() && negotiate_maxframerate.load()) {
        BOOST_LOG(warning) << "[portalgrab] Negotiation failed, will retry without maxFramerate"sv;
        negotiate_maxframerate.store(false);
        pipewire.set_negotiate_maxframerate(false);
        out_status = platf::capture_e::reinit;
        return true;  // Stop capture with reinit (due to out_status)
      }
      return false;  // Return to default stream dead handling
    }

    // DBus portal connection
    dbus_t dbus;  ///< DBus connection used for portal screencast requests.

    // Class variable to store runtime state of maxFramerate negotiation
    static inline std::atomic negotiate_maxframerate {true};  ///< Whether portal negotiation should request the maximum frame rate.
  };

#ifdef SUNSHINE_TESTS
  /**
   * @brief Exercise the portal's dead-stream handling without opening a real portal session.
   *
   * @param shared_state Stream state populated by PipeWire callbacks.
   * @param negotiate_maxframerate Negotiation setting before and after handling the dead stream.
   * @param out_status Capture status selected by the handler.
   * @return True when portal-specific handling selected a capture status.
   */
  bool check_stream_dead_for_testing(std::shared_ptr<pipewire::shared_state_t> shared_state, bool &negotiate_maxframerate, platf::capture_e &out_status) {
    /**
     * @brief Portal display with injected callback state and no D-Bus session.
     */
    class test_portal_t: public portal_t {
    public:
      /**
       * @brief Install the stream state to be examined by the portal handler.
       * @param state Stream state populated by PipeWire callbacks.
       */
      explicit test_portal_t(std::shared_ptr<pipewire::shared_state_t> state):
          portal_t {} {
        this->shared_state = std::move(state);
        dbus.pipewire_fd = -1;
      }
    } display {std::move(shared_state)};

    const bool saved = portal_t::negotiate_maxframerate.exchange(negotiate_maxframerate);
    auto restore = util::fail_guard([saved]() {
      portal_t::negotiate_maxframerate.store(saved);
    });
    const bool handled = display.check_stream_dead(out_status);
    negotiate_maxframerate = portal_t::negotiate_maxframerate.load();
    return handled;
  }
#endif
}  // namespace portal

namespace platf {
  /**
   * @brief Create a portal-based display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend backed by xdg-desktop-portal and PipeWire, or nullptr.
   */
  std::shared_ptr<display_t> portal_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    using enum platf::mem_type_e;
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[portalgrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto portal = std::make_shared<portal::portal_t>();
    if (portal->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return portal;
  }

  /**
   * @brief Enumerate capture targets available through xdg-desktop-portal.
   *
   * @param allow_start_timeout True if "Start" DBus call is allowed to time out.
   * @return Portal display names, or an empty list when portal discovery fails.
   */
  std::vector<std::string> portal_display_names(bool allow_start_timeout) {
    std::vector<std::string> display_names;
    auto dbus = std::make_shared<portal::dbus_t>();

    if (dbus->init() < 0) {
      BOOST_LOG(warning) << "[portalgrab] Failed to connect to dbus. Cannot enumerate displays, returning empty list.";
      return {};
    }

    if (dbus->connect_to_portal(allow_start_timeout) < 0) {
      BOOST_LOG(warning) << "[portalgrab] Failed to connect to portal. Cannot enumerate displays, returning empty list.";
      return {};
    }

    for (auto stream_ : dbus->pipewire_streams) {
      BOOST_LOG(info) << "[portalgrab] Found stream for display id/name: '"sv << stream_.monitor_name << "' position: "sv << stream_.pos_x << "x"sv << stream_.pos_y << " resolution: "sv << stream_.width << "x"sv << stream_.height;
      display_names.emplace_back(stream_.to_display_name());
    }
    // Release the portal session as soon as possible to properly release related resources early.
    dbus.reset();

    // Return currently active display names
    return display_names;
  }
}  // namespace platf
