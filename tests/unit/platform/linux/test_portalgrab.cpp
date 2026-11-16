/**
 * @file tests/unit/platform/linux/test_portalgrab.cpp
 * @brief Test XDG portal request handling against a fake portal on a private D-Bus.
 */
#ifdef SUNSHINE_BUILD_PORTAL
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>
  #include <atomic>
  #include <csignal>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <format>
  #include <fstream>
  #include <functional>
  #include <future>
  #include <map>
  #include <optional>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  // lib includes
  #include <fcntl.h>
  #include <gio/gio.h>
  #include <gio/gunixfdlist.h>
  #include <poll.h>
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include "src/logging.h"
  #include "src/platform/linux/misc.h"

namespace platf {
  std::vector<std::string> portal_display_names(bool allow_start_timeout);
}  // namespace platf

namespace {
  constexpr const char *PORTAL_NAME = "org.freedesktop.portal.Desktop";
  constexpr const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";

  /**
   * @brief Portal methods used by Sunshine to set up a RemoteDesktop session.
   */
  constexpr const char *PORTAL_XML = R"(
<node>
  <interface name='org.freedesktop.portal.RemoteDesktop'>
    <method name='CreateSession'>
      <arg type='a{sv}' direction='in'/>
      <arg type='o' direction='out'/>
    </method>
    <method name='SelectDevices'>
      <arg type='o' direction='in'/>
      <arg type='a{sv}' direction='in'/>
      <arg type='o' direction='out'/>
    </method>
    <method name='Start'>
      <arg type='o' direction='in'/>
      <arg type='s' direction='in'/>
      <arg type='a{sv}' direction='in'/>
      <arg type='o' direction='out'/>
    </method>
  </interface>
  <interface name='org.freedesktop.portal.ScreenCast'>
    <method name='SelectSources'>
      <arg type='o' direction='in'/>
      <arg type='a{sv}' direction='in'/>
      <arg type='o' direction='out'/>
    </method>
    <method name='OpenPipeWireRemote'>
      <arg type='o' direction='in'/>
      <arg type='a{sv}' direction='in'/>
      <arg type='h' direction='out'/>
    </method>
  </interface>
</node>)";

  /**
   * @brief Fake xdg-desktop-portal that emits each Request::Response before returning the request handle.
   *
   * A client that subscribes to Response only after the method call returns never sees these responses,
   * which makes the race with a fast real portal deterministic.
   */
  class fake_portal_t {
  public:
    /**
     * @brief Serve the portal on the given bus from a background thread.
     *
     * @param bus_address Address of the private test bus.
     * @return True when the portal owns its name and is ready for requests.
     */
    bool start(const std::string &bus_address) {
      context_ = g_main_context_new();
      loop_ = g_main_loop_new(context_, false);

      std::promise<bool> ready;
      auto started = ready.get_future();
      thread_ = std::jthread([this, bus_address, ready = std::move(ready)]() mutable {
        g_main_context_push_thread_default(context_);
        const bool ok = setup(bus_address);
        ready.set_value(ok);
        if (ok) {
          g_main_loop_run(loop_);
        }
        for (const auto id : registrations_) {
          g_dbus_connection_unregister_object(conn_, id);
        }
        g_clear_object(&conn_);
        g_main_context_pop_thread_default(context_);
      });
      return started.get();
    }

    /**
     * @brief Stop serving requests and release the portal's resources.
     */
    void stop() {
      if (thread_.joinable()) {
        g_main_context_invoke(
          context_,
          [](gpointer loop) {
            g_main_loop_quit(static_cast<GMainLoop *>(loop));
            return G_SOURCE_REMOVE;
          },
          loop_
        );
        thread_.join();
      }
      if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
      }
      if (context_) {
        g_main_context_unref(context_);
        context_ = nullptr;
      }
    }

    /**
     * @brief Stop the portal if the test did not.
     */
    ~fake_portal_t() {
      stop();
    }

    /**
     * @brief Configure whether restored sessions return no streams.
     *
     * @param reject True to make Start return no streams when the session used a restore token.
     */
    void set_reject_restored_sessions(bool reject) {
      reject_restored_sessions_.store(reject);
    }

    /**
     * @brief Check whether any request carried a restore token.
     *
     * @return True when a restore token was seen.
     */
    bool saw_restore_token() const {
      return saw_restore_token_.load();
    }

  private:
    /**
     * @brief Connect to the bus, claim the portal name and export the portal object.
     */
    bool setup(const std::string &bus_address) {
      conn_ = g_dbus_connection_new_for_address_sync(
        bus_address.c_str(),
        GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr,
        nullptr,
        nullptr
      );
      if (!conn_) {
        return false;
      }

      g_autoptr(GVariant) name_reply = g_dbus_connection_call_sync(
        conn_,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", PORTAL_NAME, 4u),  // DBUS_NAME_FLAG_DO_NOT_QUEUE
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr
      );
      guint32 name_result = 0;
      if (!name_reply) {
        return false;
      }
      g_variant_get(name_reply, "(u)", &name_result);
      if (name_result != 1) {  // DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
        return false;
      }

      g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(PORTAL_XML, nullptr);
      static const GDBusInterfaceVTable vtable {handle_method_call, nullptr, nullptr, {}};
      for (auto **iface = node->interfaces; *iface; ++iface) {
        const auto id = g_dbus_connection_register_object(conn_, PORTAL_PATH, *iface, &vtable, this, nullptr, nullptr);
        if (id == 0) {
          return false;
        }
        registrations_.push_back(id);
      }
      return true;
    }

    /**
     * @brief Answer a portal method call, emitting the request's Response first.
     */
    static void handle_method_call(GDBusConnection *conn, const gchar *sender, [[maybe_unused]] const gchar *object_path, [[maybe_unused]] const gchar *interface_name, const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data) {  // NOSONAR(cpp:S107): GDBusInterfaceMethodCallFunc signature.
      auto *portal = static_cast<fake_portal_t *>(user_data);
      if (g_str_equal(method_name, "OpenPipeWireRemote")) {
        g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new();
        const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        g_unix_fd_list_append(fd_list, fd, nullptr);
        close(fd);
        g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(h)", 0), fd_list);
        return;
      }

      // The options vardict is the last argument of every request method
      g_autoptr(GVariant) options = g_variant_get_child_value(parameters, g_variant_n_children(parameters) - 1);
      const gchar *handle_token = nullptr;
      if (!g_variant_lookup(options, "handle_token", "&s", &handle_token)) {
        g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "handle_token is required");
        return;
      }

      const gchar *restore_token = nullptr;
      const bool has_restore_token = g_variant_lookup(options, "restore_token", "&s", &restore_token) && restore_token && restore_token[0] != '\0';

      g_autofree gchar *sender_id = g_strdelimit(g_strdup(sender + 1), ".", '_');
      g_autofree gchar *request_path = g_strdup_printf("%s/request/%s/%s", PORTAL_PATH, sender_id, handle_token);

      GVariantBuilder results;
      g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
      if (g_str_equal(method_name, "CreateSession")) {
        const gchar *session_token = nullptr;
        g_variant_lookup(options, "session_handle_token", "&s", &session_token);
        g_autofree gchar *session_path = g_strdup_printf("%s/session/%s/%s", PORTAL_PATH, sender_id, session_token);
        portal->session_restore_tokens_[session_path] = false;
        g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(session_path));
      } else {
        g_autoptr(GVariant) session_value = g_variant_get_child_value(parameters, 0);
        const gchar *session_path = g_variant_get_string(session_value, nullptr);
        if (g_str_equal(method_name, "SelectDevices") || g_str_equal(method_name, "SelectSources")) {
          if (has_restore_token) {
            portal->session_restore_tokens_[session_path] = true;
            portal->saw_restore_token_.store(true);
          }
        } else if (g_str_equal(method_name, "Start")) {
          const auto session = portal->session_restore_tokens_.find(session_path);
          const bool restored_session = session != portal->session_restore_tokens_.end() && session->second;
          if (!restored_session || !portal->reject_restored_sessions_.load()) {
            // A geometry no real monitor has, so the stream is named by position and resolution
            GVariantBuilder properties;
            g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&properties, "{sv}", "size", g_variant_new("(ii)", 1234, 567));
            g_variant_builder_add(&properties, "{sv}", "position", g_variant_new("(ii)", 7, 9));
            GVariantBuilder streams;
            g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
            g_variant_builder_add(&streams, "(ua{sv})", 42u, &properties);
            g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
          }
          if (!restored_session) {
            g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string("fresh-token"));
          }
        }
      }

      g_dbus_connection_emit_signal(conn, sender, request_path, "org.freedesktop.portal.Request", "Response", g_variant_new("(ua{sv})", 0u, &results), nullptr);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request_path));
    }

    GMainContext *context_ = nullptr;
    GMainLoop *loop_ = nullptr;
    GDBusConnection *conn_ = nullptr;
    std::vector<guint> registrations_;
    std::map<std::string, bool> session_restore_tokens_;
    std::atomic<bool> saw_restore_token_ {false};
    std::atomic<bool> reject_restored_sessions_ {true};
    std::jthread thread_;
  };

  /**
   * @brief Configuration of the private test bus, matching the one GLib's GTestDBus writes.
   */
  constexpr const char *BUS_CONFIG = R"(<busconfig>
  <type>session</type>
  <listen>unix:dir={}</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
)";

  /**
   * @brief Run each test against a fake portal on a private session bus.
   *
   * The portal code connects to the session bus named by DBUS_SESSION_BUS_ADDRESS, so the fixture points it at a
   * private dbus-daemon for the duration of the test. GTestDBus is not used because tearing it down releases the
   * process-wide session bus connection that other tests may share.
   */
  class PortalRequestTest: public BaseTest {
  protected:
    /**
     * @brief Start a private session bus and the fake portal on it.
     */
    void SetUp() override {
      BaseTest::SetUp();
      if (HasFatalFailure() || IsSkipped()) {
        return;
      }
      g_autofree gchar *daemon = g_find_program_in_path("dbus-daemon");
      if (!daemon) {
        GTEST_SKIP() << "dbus-daemon is required for a private test bus";
      }

      auto directory_template = (std::filesystem::temp_directory_path() / "sunshine-portal-bus-XXXXXX").string();  // NOSONAR(cpp:S5443): mkdtemp creates a private unique directory.
      ASSERT_NE(mkdtemp(directory_template.data()), nullptr);
      bus_directory_ = directory_template;
      token_path_ = bus_directory_ / "portal_token.test";
      portal::set_saved_token_path_for_testing(token_path_);
      portal::clear_saved_token();

      const auto config_path = bus_directory_ / "bus.conf";
      std::ofstream(config_path) << std::format(BUS_CONFIG, bus_directory_.string());

      std::string config_arg = "--config-file=" + config_path.string();
      std::string nofork_arg = "--nofork";
      std::string address_arg = "--print-address";
      std::array<gchar *, 5> argv {daemon, config_arg.data(), nofork_arg.data(), address_arg.data(), nullptr};
      gint output_fd = -1;
      ASSERT_TRUE(g_spawn_async_with_pipes(nullptr, argv.data(), nullptr, G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, &daemon_pid_, nullptr, &output_fd, nullptr, nullptr));
      daemon_output_ = fdopen(output_fd, "r");
      ASSERT_NE(daemon_output_, nullptr);

      pollfd output_poll {output_fd, POLLIN, 0};
      ASSERT_EQ(poll(&output_poll, 1, 5000), 1) << "dbus-daemon did not print its address within 5 seconds";
      std::array<char, 512> address {};
      ASSERT_NE(fgets(address.data(), address.size(), daemon_output_), nullptr) << "dbus-daemon did not print its address";
      const std::string bus_address = g_strchomp(address.data());

      // Drain queued log records first: the log formatter reads the environment, which must not race with setenv
      logging::log_flush();
      set_env("DBUS_SESSION_BUS_ADDRESS", bus_address);
      // Start resolves monitor names through the compositor; keep the test away from the user's session.
      set_env("WAYLAND_DISPLAY", (bus_directory_ / "no-wayland").string());

      ASSERT_TRUE(portal_.start(bus_address));
    }

    /**
     * @brief Stop the fake portal and the private bus, and restore the environment.
     */
    void TearDown() override {
      portal_.stop();
      if (!token_path_.empty()) {
        portal::clear_saved_token();
        portal::reset_saved_token_path_for_testing();
      }
      logging::log_flush();
      for (const auto &[name, previous] : previous_env_) {
        if (previous) {
          g_setenv(name.c_str(), previous->c_str(), true);
        } else {
          g_unsetenv(name.c_str());
        }
      }
      if (daemon_pid_ > 0) {
        kill(daemon_pid_, SIGTERM);
        waitpid(daemon_pid_, nullptr, 0);
        g_spawn_close_pid(daemon_pid_);
      }
      if (daemon_output_) {
        fclose(daemon_output_);
      }
      if (!bus_directory_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(bus_directory_, ec);
      }
      BaseTest::TearDown();
    }

    /**
     * @brief Get the private token path used by this test.
     *
     * @return Token path inside the private bus directory.
     */
    const std::filesystem::path &token_path() const {
      return token_path_;
    }

    /**
     * @brief Write a restore token to the private token path.
     *
     * @param value Token contents.
     */
    void write_token(const std::string &value) const {
      std::ofstream file(token_path_);
      ASSERT_TRUE(file.is_open());
      file << value;
      ASSERT_TRUE(file.good());
    }

    /**
     * @brief Access the fake portal used by this test.
     *
     * @return Fake portal instance.
     */
    fake_portal_t &fake_portal() {
      return portal_;
    }

  private:
    /**
     * @brief Set an environment variable for the test, remembering its previous value for TearDown.
     * @param name Variable name.
     * @param value New value.
     */
    void set_env(const std::string &name, const std::string &value) {
      if (!previous_env_.contains(name)) {
        const gchar *previous = g_getenv(name.c_str());
        previous_env_[name] = previous ? std::optional<std::string> {previous} : std::nullopt;
      }
      g_setenv(name.c_str(), value.c_str(), true);
    }

    fake_portal_t portal_;
    GPid daemon_pid_ = 0;
    FILE *daemon_output_ = nullptr;
    std::filesystem::path bus_directory_;
    std::filesystem::path token_path_;
    std::map<std::string, std::optional<std::string>, std::less<>> previous_env_;
  };
}  // namespace

TEST_F(PortalRequestTest, ReceivesResponseEmittedBeforeMethodReturns) {
  const auto names = platf::portal_display_names(true);

  EXPECT_EQ(names, std::vector<std::string> {"position-7x9-resolution-1234x567"});
}

TEST_F(PortalRequestTest, DiscardsStaleRestoreTokenWhenStartReturnsNoStreams) {
  ASSERT_NO_FATAL_FAILURE(write_token("stale-token"));
  ASSERT_TRUE(std::filesystem::exists(token_path()));

  const auto names = platf::portal_display_names(true);

  EXPECT_EQ(names, std::vector<std::string> {"position-7x9-resolution-1234x567"});
  EXPECT_TRUE(fake_portal().saw_restore_token());
  ASSERT_TRUE(std::filesystem::exists(token_path()));
  std::ifstream file(token_path());
  std::string token;
  std::getline(file, token);
  EXPECT_EQ(token, "fresh-token");
  EXPECT_TRUE(portal::has_saved_token());
}

TEST_F(PortalRequestTest, KeepsRestoreTokenWhenStartReturnsStreams) {
  fake_portal().set_reject_restored_sessions(false);
  ASSERT_NO_FATAL_FAILURE(write_token("valid-token"));

  const auto names = platf::portal_display_names(true);

  EXPECT_EQ(names, std::vector<std::string> {"position-7x9-resolution-1234x567"});
  EXPECT_TRUE(fake_portal().saw_restore_token());
  ASSERT_TRUE(std::filesystem::exists(token_path()));
  std::ifstream file(token_path());
  std::string token;
  std::getline(file, token);
  EXPECT_EQ(token, "valid-token");
}
#endif
