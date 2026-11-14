/**
 * @file tests/unit/platform/linux/test_pipewire.cpp
 * @brief Test PipeWire stream lifecycle handling and capture display teardown.
 */
#if defined(SUNSHINE_BUILD_KWIN) || defined(SUNSHINE_BUILD_PORTAL)
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>
  #include <cerrno>
  #include <chrono>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <memory>
  #include <optional>
  #include <string>
  #include <thread>
  #include <utility>

  // lib includes
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <unistd.h>

  // local includes
  #include "src/logging.h"
  #include "src/platform/linux/pipewire.cpp"

using namespace std::literals;

  #ifdef SUNSHINE_BUILD_PORTAL
namespace portal {
  bool check_stream_dead_for_testing(std::shared_ptr<pipewire::shared_state_t> shared_state, bool &negotiate_maxframerate, platf::capture_e &out_status);
}  // namespace portal
  #endif

namespace {
  /**
   * @brief PipeWire display without a capture backend behind it.
   *
   * The tests create it with std::make_shared, as every backend creates its display.
   */
  class test_display_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream([[maybe_unused]] const std::string &display_name, [[maybe_unused]] int &out_pipewire_fd, [[maybe_unused]] uint32_t &out_pipewire_node, [[maybe_unused]] uint64_t &out_pipewire_objectserial) override {
      return -1;
    }

    /**
     * @brief Connect the display's PipeWire core to a socket, as for a stream from the portal.
     *
     * @param fd Connected socket. The display owns it afterwards.
     * @param state Callback state for the new connection.
     * @return Result of pipewire_t::init(): 0 on success.
     */
    int connect(int fd, std::shared_ptr<pipewire::shared_state_t> state = std::make_shared<pipewire::shared_state_t>()) {
      return pipewire.init(fd, PW_ID_ANY, SPA_ID_INVALID, std::move(state));
    }
  };

  /**
   * @brief Check whether a file descriptor is open.
   *
   * @param fd File descriptor to check.
   * @return True when fd refers to an open file.
   */
  bool is_open(int fd) {
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
  }

  /**
   * @brief Check whether PipeWire can create a client context with the installed configuration.
   *
   * pw_context_new() fails when client.conf is not installed, for example where only the PipeWire library is.
   *
   * @return True when a context can be created.
   */
  bool can_create_pipewire_context() {
    pw_loop *loop = pw_loop_new(nullptr);
    if (!loop) {
      return false;
    }
    pw_context *context = pw_context_new(loop, nullptr, 0);
    const bool created = context != nullptr;
    if (context) {
      pw_context_destroy(context);
    }
    pw_loop_destroy(loop);
    return created;
  }

  /**
   * @brief Set an environment variable while the object lives, then restore the previous value.
   *
   * The display logs right before and inside the scope. Draining those records first prevents the logging formatter
   * from reading environment-dependent state concurrently with `setenv()` or `unsetenv()` on older glibc releases.
   */
  class scoped_env_t {
  public:
    /**
     * @brief Save the previous environment value and install a temporary replacement.
     *
     * @param name Environment variable to override.
     * @param value Temporary value used while this guard exists.
     */
    scoped_env_t(const char *name, const std::string &value):
        name_ {name} {
      logging::log_flush();
      if (const char *previous = std::getenv(name)) {  // NOSONAR(cpp:S1874): `_dupenv_s` not available
        previous_ = previous;
      }
      setenv(name, value.c_str(), 1);
    }

    /**
     * @brief Restore the saved value or remove a variable that was originally absent.
     */
    ~scoped_env_t() {
      logging::log_flush();
      if (previous_) {
        setenv(name_, previous_->c_str(), 1);
      } else {
        unsetenv(name_);
      }
    }

    /**
     * @brief Disable copying of an active environment override.
     */
    scoped_env_t(const scoped_env_t &) = delete;

    /**
     * @brief Disable assignment of an active environment override.
     */
    scoped_env_t &operator=(const scoped_env_t &) = delete;

  private:
    const char *name_;  ///< Environment variable restored when the guard is destroyed.
    std::optional<std::string> previous_;  ///< Previous value, or no value when the variable was absent.
  };

  /**
   * @brief Destroy a display on another thread and wait for the teardown a limited time.
   *
   * A teardown that deadlocks then fails the test instead of hanging the test run.
   *
   * @param display Display to destroy. It is empty afterwards.
   * @param limit How long to wait.
   * @return True when the teardown finished in time.
   */
  bool reset_within(std::shared_ptr<test_display_t> &display, std::chrono::milliseconds limit) {
    auto finished = std::make_shared<std::promise<void>>();
    auto done = finished->get_future();
    std::jthread teardown {[display = std::move(display), finished]() mutable {
      display.reset();
      finished->set_value();
    }};
    teardown.detach();  // NOSONAR(cpp:S5962): a hung teardown must fail the test, not hang the test run
    return done.wait_for(limit) == std::future_status::ready;
  }

  /**
   * @brief Let init() fail with the given PipeWire client configuration, then check that teardown finishes and closes the stream fd.
   *
   * @param client_conf Content of client.conf, or no value to leave client.conf out.
   */
  void expect_failed_init_closes_stream_fd(const std::optional<std::string> &client_conf) {
    auto directory_template = (std::filesystem::temp_directory_path() / "sunshine-test-pipewire-XXXXXX").string();  // NOSONAR(cpp:S5443): mkdtemp creates a private unique directory.
    const auto *directory = mkdtemp(directory_template.data());
    ASSERT_NE(directory, nullptr);
    const std::filesystem::path config_dir {directory};
    if (client_conf) {
      std::ofstream config {config_dir / "client.conf"};
      config << *client_conf;
    }

    std::array<int, 2> sockets {};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
    const int stream_fd = sockets[0];
    struct stat stream {};
    ASSERT_EQ(fstat(stream_fd, &stream), 0);

    auto display = std::make_shared<test_display_t>();
    {
      scoped_env_t config_env {"PIPEWIRE_CONFIG_DIR", config_dir.string()};
      EXPECT_NE(display->connect(stream_fd), 0);
    }
    std::filesystem::remove_all(config_dir);

    ASSERT_TRUE(reset_within(display, 5s)) << "the display teardown after a failed init() did not finish";

    // PipeWire did not take the stream fd over, so the display must have closed it.
    struct stat after {};
    const bool leaked = fstat(stream_fd, &after) == 0 && after.st_dev == stream.st_dev && after.st_ino == stream.st_ino;
    EXPECT_FALSE(leaked) << "the display left open the stream fd that PipeWire did not take over";
    if (leaked) {
      close(stream_fd);
    }
    close(sockets[1]);
  }

  /**
   * @brief Initialize PipeWire as the capture backends do before they create a display.
   */
  class PipeWireDisplayTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      ASSERT_TRUE(pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(platf::mem_type_e::system));
    }
  };
}  // namespace

TEST(PipeWireStreamStateTest, RecordsStartupUntilTheStreamIsReplaced) {
  pipewire::stream_data_t data {};
  data.shared = std::make_shared<pipewire::shared_state_t>();
  EXPECT_FALSE(data.shared->has_streamed.load());

  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_PAUSED, nullptr);
  EXPECT_FALSE(data.shared->has_streamed.load());
  EXPECT_FALSE(data.shared->stream_dead.load());

  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING, nullptr);
  EXPECT_TRUE(data.shared->has_streamed.load());
  EXPECT_FALSE(data.shared->stream_dead.load());

  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR, "no more input formats");
  EXPECT_TRUE(data.shared->has_streamed.load());
  EXPECT_TRUE(data.shared->stream_dead.exchange(false));

  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_ERROR, PW_STREAM_STATE_UNCONNECTED, nullptr);
  EXPECT_TRUE(data.shared->has_streamed.load());
  EXPECT_TRUE(data.shared->stream_dead.load());
}

TEST(PipeWireStreamStateTest, StreamingWithoutSharedStateIsIgnored) {
  pipewire::stream_data_t data {};
  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING, nullptr);
  EXPECT_FALSE(data.shared);
}

  #ifdef SUNSHINE_BUILD_PORTAL
TEST_F(PipeWireDisplayTest, PortalRetainsMaxFramerateAfterAnEstablishedStreamFails) {
  for (const bool handle_pause_first : {false, true}) {
    SCOPED_TRACE(handle_pause_first);
    pipewire::stream_data_t data {};
    data.shared = std::make_shared<pipewire::shared_state_t>();
    bool negotiate_maxframerate = true;
    auto status = platf::capture_e::ok;

    pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING, nullptr);
    pw_buffer buffer {};
    data.current_buffer = &buffer;
    data.frame_ready = true;
    pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_PAUSED, nullptr);
    EXPECT_FALSE(data.frame_ready);
    EXPECT_EQ(data.current_buffer, nullptr);
    ASSERT_TRUE(data.shared->stream_dead.load());

    // The capture thread may handle the pause before or after PipeWire reports the later error.
    if (handle_pause_first) {
      EXPECT_TRUE(data.shared->stream_dead.exchange(false));
      EXPECT_FALSE(portal::check_stream_dead_for_testing(data.shared, negotiate_maxframerate, status));
      EXPECT_TRUE(negotiate_maxframerate);
      EXPECT_EQ(status, platf::capture_e::ok);
    }

    pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_ERROR, "no more input formats");
    ASSERT_TRUE(data.shared->stream_dead.exchange(false));
    EXPECT_FALSE(portal::check_stream_dead_for_testing(data.shared, negotiate_maxframerate, status));
    EXPECT_TRUE(negotiate_maxframerate);
    EXPECT_EQ(status, platf::capture_e::ok);
  }
}

TEST_F(PipeWireDisplayTest, PortalRetriesWithoutMaxFramerateOnlyIfTheStreamNeverStarted) {
  pipewire::stream_data_t data {};
  data.shared = std::make_shared<pipewire::shared_state_t>();
  bool negotiate_maxframerate = true;
  auto status = platf::capture_e::ok;

  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_PAUSED, nullptr);
  pipewire::pipewire_t::on_stream_state_changed(&data, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_ERROR, "no more input formats");
  ASSERT_TRUE(data.shared->stream_dead.exchange(false));
  EXPECT_TRUE(portal::check_stream_dead_for_testing(data.shared, negotiate_maxframerate, status));
  EXPECT_FALSE(negotiate_maxframerate);
  EXPECT_EQ(status, platf::capture_e::reinit);

  status = platf::capture_e::ok;
  EXPECT_FALSE(portal::check_stream_dead_for_testing(data.shared, negotiate_maxframerate, status));
  EXPECT_FALSE(negotiate_maxframerate);
  EXPECT_EQ(status, platf::capture_e::ok);
}
  #endif

TEST_F(PipeWireDisplayTest, NewConnectionResetsStartupHistory) {
  std::array<int, 2> sockets {};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
  auto state = std::make_shared<pipewire::shared_state_t>();
  state->has_streamed.store(true);
  auto display = std::make_shared<test_display_t>();
  display->connect(sockets[0], state);
  EXPECT_FALSE(state->has_streamed.load());
  display.reset();
  close(sockets[1]);
}

TEST_F(PipeWireDisplayTest, DisplayDestroyedBeforeInitKeepsStandardInput) {
  // Make sure fd 0 is open, so that closing it is visible even when the runner closed standard input.
  int opened_stdin = -1;
  if (!is_open(STDIN_FILENO)) {
    opened_stdin = open("/dev/null", O_RDONLY);  // NOSONAR(cpp:S1874): `_sopen_s` not available
    ASSERT_EQ(opened_stdin, STDIN_FILENO);
  }
  const int saved_stdin = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
  ASSERT_GE(saved_stdin, 0);
  struct stat before {};
  ASSERT_EQ(fstat(STDIN_FILENO, &before), 0);

  // configure_stream() failed, so init() never ran: the display owns no descriptor.
  auto display = std::make_shared<test_display_t>();
  display.reset();

  struct stat after {};
  const bool stdin_open = fstat(STDIN_FILENO, &after) == 0;
  EXPECT_TRUE(stdin_open) << "the display closed fd 0";
  if (stdin_open) {
    EXPECT_EQ(after.st_dev, before.st_dev);
    EXPECT_EQ(after.st_ino, before.st_ino);
  }

  // Leave fd 0 as the test found it, also after a failure.
  dup2(saved_stdin, STDIN_FILENO);
  close(saved_stdin);
  if (opened_stdin >= 0) {
    close(opened_stdin);
  }
}

TEST_F(PipeWireDisplayTest, TeardownDoesNotCloseFdThatPipeWireClosed) {
  if (!can_create_pipewire_context()) {
    GTEST_SKIP() << "PipeWire cannot create a client context; client.conf is probably not installed";
  }

  // Opened first, so that it cannot take the number of the stream fd later.
  const int placeholder = open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(placeholder, 0);
  std::array<int, 2> sockets {};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
  const int stream_fd = sockets[0];

  auto display = std::make_shared<test_display_t>();
  ASSERT_EQ(display->connect(stream_fd), 0);

  // The remote end goes away. PipeWire then closes the stream fd on its own thread.
  close(sockets[1]);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (is_open(stream_fd) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_FALSE(is_open(stream_fd)) << "PipeWire did not close the stream fd after the remote end went away";

  // Another part of the process opens a file and gets the free number.
  const int reused = fcntl(placeholder, F_DUPFD_CLOEXEC, stream_fd);
  close(placeholder);
  ASSERT_EQ(reused, stream_fd);

  display.reset();

  EXPECT_TRUE(is_open(reused)) << "the display closed a descriptor that it no longer owned";
  close(reused);
}

TEST_F(PipeWireDisplayTest, TeardownAfterFailedContextClosesStreamFd) {
  // Without client.conf, pw_context_new() fails.
  expect_failed_init_closes_stream_fd(std::nullopt);
}

TEST_F(PipeWireDisplayTest, TeardownAfterFailedConnectionClosesStreamFd) {
  // With a client.conf that loads no modules, PipeWire creates the context, but without the native protocol
  // pw_context_connect_fd() fails and does not take the stream fd over.
  expect_failed_init_closes_stream_fd("context.properties = { }\ncontext.modules = [ ]\n");
}
#endif
