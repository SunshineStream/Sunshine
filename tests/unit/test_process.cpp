/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/process/v1.hpp>

#ifdef _WIN32
  // lib includes
  #include <boost/process/v1/windows.hpp>
  #include <Windows.h>
#else
  // lib includes
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

// local includes
#include <src/process.h>
#include <src/utility.h>

namespace fs = std::filesystem;

namespace proc {
  /**
   * @brief Resolve a command's working directory, returning an empty path for URLs.
   *
   * @param cmd Command line to inspect.
   * @param env Environment used for executable lookup.
   * @return Working directory, or an empty path when the command is a URL.
   */
  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env);

  /**
   * @brief Expand environment references while preserving Windows variable-name casing.
   *
   * @param env Environment whose variables are expanded.
   * @param val_raw Text containing variable references.
   * @return Text with environment references expanded.
   */
  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw);

  /**
   * @brief Check a prep-command launch and wait for successful completion.
   *
   * @param child Process returned by the launch, or an empty child on failure.
   * @param command Command line used for logging.
   * @param ec Launch error, updated by the wait or cleared for an allowed permission denial.
   * @param allow_permission_denied Whether permission-denied launches may be skipped for Desktop access.
   * @return True if the command succeeded or its permission-denied launch was allowed.
   */
  bool wait_for_prep_command(boost::process::v1::child &child, const std::string &command, std::error_code &ec, bool allow_permission_denied);
}  // namespace proc

namespace {
  /**
   * @brief Launch a short-lived shell command, hidden on Windows.
   *
   * @param exit_code Exit status returned by the child.
   * @param ec Error set if launching the child fails.
   * @return Child process used to exercise prep-command waiting.
   */
  boost::process::v1::child launch_prep_test_child(int exit_code, std::error_code &ec) {
    const auto command = std::format("exit {}", exit_code);
#ifdef _WIN32
    return boost::process::v1::child(boost::process::v1::search_path("cmd.exe"), "/c", command, boost::process::v1::windows::create_no_window, ec);
#else
    return boost::process::v1::child("/bin/sh", "-c", command, ec);
#endif
  }
}  // namespace

TEST(ProcessTest, DesktopSkipsPermissionDeniedPrepLaunches) {
  boost::process::v1::child child;
  auto ec = std::make_error_code(std::errc::permission_denied);

  EXPECT_TRUE(proc::wait_for_prep_command(child, "prep-command", ec, true));
  EXPECT_FALSE(ec) << "The skipped launch error must not prevent subsequent commands from starting";
  EXPECT_FALSE(child.valid());
}

TEST(ProcessTest, ApplicationsRejectPermissionDeniedPrepLaunches) {
  boost::process::v1::child child;
  auto ec = std::make_error_code(std::errc::permission_denied);

  EXPECT_FALSE(proc::wait_for_prep_command(child, "prep-command", ec, false));
  EXPECT_EQ(ec, std::errc::permission_denied);
}

TEST(ProcessTest, OtherPrepLaunchErrorsRemainFatal) {
  for (const bool desktop : {false, true}) {
    boost::process::v1::child child;
    auto ec = std::make_error_code(std::errc::no_such_file_or_directory);

    EXPECT_FALSE(proc::wait_for_prep_command(child, "prep-command", ec, desktop));
    EXPECT_EQ(ec, std::errc::no_such_file_or_directory);
  }
}

TEST(ProcessTest, PrepCommandsRunAfterSkippedPermissionDenial) {
  boost::process::v1::child skipped_child;
  auto ec = std::make_error_code(std::errc::permission_denied);
  ASSERT_TRUE(proc::wait_for_prep_command(skipped_child, "skipped-command", ec, true));
  ASSERT_FALSE(ec);

  auto child = launch_prep_test_child(0, ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_TRUE(child.valid());
  EXPECT_TRUE(proc::wait_for_prep_command(child, "prep-command", ec, true));
  EXPECT_FALSE(ec);
}

TEST(ProcessTest, NonzeroPrepExitRemainsFatalForDesktop) {
  std::error_code ec;
  auto child = launch_prep_test_child(7, ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_TRUE(child.valid());
  EXPECT_FALSE(proc::wait_for_prep_command(child, "prep-command", ec, true));
  EXPECT_FALSE(ec);
  EXPECT_EQ(child.exit_code(), 7);
}

TEST(ProcessTest, PrepWaitErrorsRemainFatalForDesktop) {
#ifdef _WIN32
  std::error_code launch_ec;
  auto launched = launch_prep_test_child(0, launch_ec);
  ASSERT_FALSE(launch_ec) << launch_ec.message();
  ASSERT_TRUE(launched.valid());

  // Keep the exited process alive with a handle that allows waiting but cannot query its exit code.
  const auto wait_only_handle = OpenProcess(SYNCHRONIZE, FALSE, launched.id());
  ASSERT_NE(wait_only_handle, nullptr);
  const boost::winapi::PROCESS_INFORMATION_ process_info {wait_only_handle, nullptr, launched.id(), 0};
  boost::process::v1::child child {boost::process::v1::child::child_handle {process_info}};
  child.detach();
  launched.wait(launch_ec);
  ASSERT_FALSE(launch_ec) << launch_ec.message();
#else
  // The current process is not our child, so waitpid() must fail without blocking.
  boost::process::v1::child child {static_cast<boost::process::v1::pid_t>(getpid())};
#endif
  std::error_code ec;
  EXPECT_FALSE(proc::wait_for_prep_command(child, "prep-command", ec, true));
  EXPECT_TRUE(ec);
}

TEST(ProcessTest, PrepLaunchErrorsAbortDesktopLaunch) {
  proc::ctx_t app {};
  app.id = "42";
  app.name = "Desktop";
  app.working_dir = SUNSHINE_SOURCE_DIR;
  app.prep_cmds.emplace_back("sunshine-nonexistent-prep-command", "", false);
  boost::process::v1::environment env = boost::this_process::environment();
  proc::proc_t process(std::move(env), {std::move(app)});
  const auto cleanup = util::fail_guard([&process]() {
    process.terminate();
  });

  EXPECT_EQ(process.execute(42, std::make_shared<rtsp_stream::launch_session_t>()), -1);
  EXPECT_EQ(process.running(), 0);
}

#ifdef _WIN32
TEST(ProcessTest, ExpandsEnvironmentNamesWithoutChangingTheirCase) {
  auto env = boost::this_process::environment();
  const std::string key = "SUNSHINE_RANGE_SEARCH_REGRESSION";
  const auto original = env.find(key);
  const std::optional<std::string> saved = original == env.end() ? std::nullopt : std::optional {original->to_string()};
  const auto restore = util::fail_guard([&env, &key, &saved]() {
    if (saved.has_value()) {
      env[key] = *saved;
    } else {
      env[key].clear();
    }
  });
  env[key] = "case-preserved";

  EXPECT_EQ(proc::parse_env_val(env, "before-$(sunshine_range_search_regression)-after"), "before-case-preserved-after");
  EXPECT_EQ(proc::parse_env_val(env, "$(SUNSHINE_RANGE_SEARCH_REGRESSION)"), "case-preserved");
}
#endif

TEST(ProcessTest, UrlCommandsDoNotResolveAnExecutableWorkingDirectory) {
  boost::process::v1::environment env;
  EXPECT_TRUE(proc::find_working_directory("https://example.com", env).empty());
  EXPECT_TRUE(proc::find_working_directory("steam://run/123", env).empty());
  EXPECT_TRUE(proc::find_working_directory("custom+scheme://host/path", env).empty());
}

#ifndef _WIN32
TEST(ProcessTest, UnavailableLocaleDoesNotAbortCommandLookup) {
  const char *original = std::getenv("LC_ALL");
  const std::optional<std::string> saved = original ? std::optional<std::string> {original} : std::nullopt;
  const auto restore = util::fail_guard([&saved]() {
    if (saved.has_value()) {
      setenv("LC_ALL", saved->c_str(), 1);
    } else {
      unsetenv("LC_ALL");
    }
  });
  setenv("LC_ALL", "xx_INVALID.UTF-8", 1);

  boost::process::v1::environment env;
  env["PATH"] = "/usr/bin:/bin";

  boost::filesystem::path working_dir;
  EXPECT_NO_THROW(working_dir = proc::find_working_directory("sh -c true", env));
  EXPECT_FALSE(working_dir.empty());
}

TEST(ProcessTest, ResolvesRelativeCommandsFromTheChildPath) {
  const auto bin_dir = fs::temp_directory_path() / "sunshine_find_working_directory_test";  // NOSONAR(cpp:S5443): safe for tests
  fs::create_directories(bin_dir);
  const auto cleanup = util::fail_guard([&bin_dir]() {
    std::error_code ec;
    fs::remove_all(bin_dir, ec);
  });
  const auto executable = bin_dir / "sunshine-path-probe";
  std::ofstream(executable) << "#!/bin/sh\n";
  fs::permissions(executable, fs::perms::owner_all);

  boost::process::v1::environment env;
  env["PATH"] = bin_dir.string();

  EXPECT_EQ(proc::find_working_directory("sunshine-path-probe --flag", env), boost::filesystem::path(bin_dir.string()));
  EXPECT_TRUE(proc::find_working_directory("sunshine-missing-probe", env).empty());
}
#endif

TEST(ProcessTest, PrepareCommand) {
#ifdef SUNSHINE_BUILD_FLATPAK
  EXPECT_EQ(proc::prepare_command("steam"), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command("flatpak-spawn --host steam"), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command("  flatpak-spawn --host steam  "), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command("  steam  "), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command(""), "");
  EXPECT_EQ(proc::prepare_command("  \t"), "");
#else
  EXPECT_EQ(proc::prepare_command("steam"), "steam");
  EXPECT_EQ(proc::prepare_command("  steam  "), "  steam  ");
  EXPECT_EQ(proc::prepare_command("flatpak-spawn --host steam"), "flatpak-spawn --host steam");
  EXPECT_EQ(proc::prepare_command(""), "");
#endif
}

class ProcessPNGTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Create test directory
    test_dir = fs::temp_directory_path() / "sunshine_process_png_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  // Helper function to create a file with specific content
  void createTestFile(const fs::path &path, const std::vector<unsigned char> &content) const {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(content.data()), content.size());
    file.close();
  }

  fs::path test_dir;
};

// Tests for check_valid_png function
TEST_F(ProcessPNGTest, CheckValidPNG_ValidSignature) {
  // Valid PNG signature
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,  // PNG signature
    // Add some dummy data to make it more realistic
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  EXPECT_TRUE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_WrongSignature) {
  // Invalid PNG signature (wrong magic bytes)
  const std::vector<unsigned char> invalid_png_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_TooShort) {
  // File too short (less than 8 bytes)
  const std::vector<unsigned char> short_data = {
    0x89,
    0x50,
    0x4E,
    0x47
  };

  const fs::path test_file = test_dir / "short.png";
  createTestFile(test_file, short_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_EmptyFile) {
  // Empty file
  const std::vector<unsigned char> empty_data = {};

  const fs::path test_file = test_dir / "empty.png";
  createTestFile(test_file, empty_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_NonExistentFile) {
  // File doesn't exist
  const fs::path test_file = test_dir / "nonexistent.png";

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_RealFile) {
  // Test with the actual sunshine.png from the project root

  // Only run this test if the file exists
  if (const fs::path sunshine_png = fs::path(SUNSHINE_SOURCE_DIR) / "sunshine.png"; fs::exists(sunshine_png)) {
    EXPECT_TRUE(proc::check_valid_png(sunshine_png));
  } else {
    GTEST_SKIP() << "sunshine.png not found in project root";
  }
}

TEST_F(ProcessPNGTest, CheckValidPNG_JPEGFile) {
  // JPEG signature (not PNG)
  const std::vector<unsigned char> jpeg_data = {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x10,
    0x4A,
    0x46
  };

  const fs::path test_file = test_dir / "fake.png";
  createTestFile(test_file, jpeg_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_PartialSignature) {
  // Partial PNG signature (first 4 bytes correct, rest wrong)
  const std::vector<unsigned char> partial_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "partial.png";
  createTestFile(test_file, partial_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

// Tests for validate_app_image_path function
TEST_F(ProcessPNGTest, ValidateAppImagePath_EmptyPath) {
  // Empty path should return default
  const std::string result = proc::validate_app_image_path("");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonPNGExtension) {
  // Non-PNG extension should return default
  const std::string result = proc::validate_app_image_path("image.jpg");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_CaseInsensitiveExtension) {
  // Test that .PNG (uppercase) is recognized
  // Create a valid PNG file
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "test.PNG";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  // Should accept uppercase .PNG extension
  EXPECT_NE(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonExistentFile) {
  // Non-existent PNG file should return default
  const std::string result = proc::validate_app_image_path("/nonexistent/path/image.png");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_InvalidPNGSignature) {
  // File with .png extension but invalid signature should return default
  const std::vector<unsigned char> invalid_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_ValidPNG) {
  // Valid PNG file should return the path
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, test_file.string());
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_OldSteamDefault) {
  // Test the special case for old steam image path
  const std::string result = proc::validate_app_image_path("./assets/steam.png");
  EXPECT_EQ(result, SUNSHINE_ASSETS_DIR "/steam.png");
}

/**
 * @brief Test fixture for proc_t::update_apps_and_env and proc::refresh.
 */
class ProcessRefreshTest: public BaseTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  void SetUp() override {
    BaseTest::SetUp();
    test_dir = fs::temp_directory_path() / "sunshine_process_refresh_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  /**
   * @brief Write a minimal valid apps.json file.
   *
   * @param path Target path for the JSON file.
   * @param app_names List of application names to include.
   */
  void writeAppsJson(const fs::path &path, const std::vector<std::string> &app_names) const {
    std::ofstream file(path);
    file << "{\n  \"env\": {},\n  \"apps\": [\n";
    for (size_t i = 0; i < app_names.size(); ++i) {
      file << R"(    { "name": ")" << app_names[i] << R"(" })";
      if (i + 1 < app_names.size()) {
        file << ",";
      }
      file << "\n";
    }
    file << "  ]\n}\n";
    file.close();

    // Artificially advance the modification time to guarantee strict monotonicity
    // across tests. This prevents the static last_apps_file_update in refresh()
    // from skipping parses when tests execute rapidly in the same clock tick.
    static int test_time_offset = 1;
    auto new_time = fs::file_time_type::clock::now() + std::chrono::seconds(test_time_offset++);
    fs::last_write_time(path, new_time);
  }

  fs::path test_dir;
};

/**
 * @brief Test fixture for testing updates to process apps and environment.
 */
class ProcessUpdateTest: public ProcessRefreshTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  proc::proc_t saved_global_proc;

  void SetUp() override {
    ProcessRefreshTest::SetUp();

    saved_global_proc = std::move(proc::proc);

    boost::process::v1::environment env = boost::this_process::environment();
    std::vector<proc::ctx_t> apps_initial;
    proc::ctx_t ctx;
    ctx.name = "Desktop";
    ctx.id = "42";
    apps_initial.push_back(std::move(ctx));

    proc::proc_t target(std::move(env), std::move(apps_initial));
    proc::proc = std::move(target);

    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
    launch_session->width = 1920;
    launch_session->height = 1080;
    launch_session->fps = 60;
    launch_session->gcmap = 0;
    launch_session->enable_hdr = false;
    launch_session->host_audio = false;
    launch_session->enable_sops = false;
    launch_session->surround_info = 2;
    int rc = proc::proc.execute(42, launch_session);
    ASSERT_EQ(rc, 0);
  }

  void TearDown() override {
    proc::proc.terminate();
    proc::proc = std::move(saved_global_proc);
    ProcessRefreshTest::TearDown();
  }

  /**
   * @brief Build a simulated source environment and app list for the update.
   *
   * @return A constructed proc_t containing the new applications and environment.
   */
  proc::proc_t buildNewSource() const {
    boost::process::v1::environment env2 = boost::this_process::environment();
    std::vector<proc::ctx_t> apps_new;
    proc::ctx_t ctx_new;
    ctx_new.name = "NewApp";
    ctx_new.id = "99";
    apps_new.push_back(std::move(ctx_new));
    return proc::proc_t(std::move(env2), std::move(apps_new));
  }
};

TEST_F(ProcessRefreshTest, UpdateAppsAndEnv_UpdatesAppsList) {
  // Build an initial proc_t with one app
  boost::process::v1::environment env = boost::this_process::environment();
  std::vector<proc::ctx_t> apps_initial;
  proc::ctx_t ctx_a;
  ctx_a.name = "AppA";
  ctx_a.id = "100";
  apps_initial.push_back(std::move(ctx_a));

  proc::proc_t target(std::move(env), std::move(apps_initial));
  ASSERT_EQ(target.get_apps().size(), 1u);
  EXPECT_EQ(target.get_apps()[0].name, "AppA");

  // Build a replacement proc_t with two apps
  boost::process::v1::environment env2 = boost::this_process::environment();
  std::vector<proc::ctx_t> apps_new;
  proc::ctx_t ctx_b;
  ctx_b.name = "AppB";
  ctx_b.id = "200";
  apps_new.push_back(std::move(ctx_b));
  proc::ctx_t ctx_c;
  ctx_c.name = "AppC";
  ctx_c.id = "300";
  apps_new.push_back(std::move(ctx_c));

  proc::proc_t source(std::move(env2), std::move(apps_new));

  // Act
  target.update_apps_and_env(std::move(source));

  // Assert — apps list must now contain the new apps
  ASSERT_EQ(target.get_apps().size(), 2u);
  EXPECT_EQ(target.get_apps()[0].name, "AppB");
  EXPECT_EQ(target.get_apps()[1].name, "AppC");
}

TEST_F(ProcessUpdateTest, AppProcessIdIsZeroForDesktopPlacebo) {
  EXPECT_EQ(proc::proc.app_process_id(), 0u);
}

#ifndef _WIN32
TEST(ProcessTest, AppProcessIdTracksRealChildLifecycle) {
  boost::process::v1::environment env = boost::this_process::environment();
  proc::ctx_t ctx {};
  ctx.name = "Sleeper";
  ctx.id = "7";
  ctx.cmd = "sleep 30";
  std::vector<proc::ctx_t> apps;
  apps.push_back(std::move(ctx));
  proc::proc_t target(std::move(env), std::move(apps));
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
  ASSERT_EQ(target.execute(7, launch_session), 0);
  const auto expected_pid = target.app_process_id();
  EXPECT_NE(expected_pid, 0u);
  std::atomic<bool> saw_unexpected_pid {false};
  std::atomic<bool> saw_dead_pid {false};
  std::atomic<unsigned int> nonzero_samples {0};
  std::latch reader_ready {1};
  std::jthread reader {[&](std::stop_token stop_token) {
    bool signaled = false;
    while (!stop_token.stop_requested()) {
      const auto pid = target.app_process_id();
      if (pid != 0u) {
        nonzero_samples.fetch_add(1);
        if (pid != expected_pid) {
          saw_unexpected_pid.store(true);
        }
        if (::kill(static_cast<pid_t>(pid), 0) != 0) {
          saw_dead_pid.store(true);
        }
      }
      if (!signaled) {
        signaled = true;
        reader_ready.count_down();
      }
    }
  }};
  reader_ready.wait();
  // Identity oracle: the reported PID must name the live `sleep` child, so a
  // constant-but-wrong PID fails even when it names some other live process.
  {
    std::ifstream cmdline {std::format("/proc/{}/cmdline", expected_pid), std::ios::binary};
    const std::string content((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("sleep"), std::string::npos) << "reported PID does not name the sleep child";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  target.terminate();
  reader.request_stop();
  reader.join();
  EXPECT_GE(nonzero_samples.load(), 10u) << "too few live samples to pin lifecycle agreement";
  EXPECT_FALSE(saw_unexpected_pid.load());
  EXPECT_FALSE(saw_dead_pid.load());
  EXPECT_EQ(target.app_process_id(), 0u);
}

#endif

/**
 * @brief Verify the provider handed to audio capture tracks the launched app.
 *
 * `stream.cpp` passes `proc::app_process_identity_provider(proc::proc)` to
 * `audio::capture()`; this pins that the provider reports the live child
 * identity and falls back to {0, 0} after termination.
 */
TEST(ProcessTest, AppProcessIdentityProviderTracksRealChildLifecycle) {
  boost::process::v1::environment env = boost::this_process::environment();
  proc::ctx_t ctx {};
  ctx.name = "Sleeper";
  ctx.id = "7";
#ifdef _WIN32
  ctx.cmd = R"(cmd.exe /c ping.exe -n 31 127.0.0.1 >NUL)";
#else
  ctx.cmd = "sleep 30";
#endif
  proc::ctx_t desktop {};
  desktop.name = "Desktop";
  desktop.id = "42";
  std::vector<proc::ctx_t> apps;
  apps.push_back(std::move(ctx));
  apps.push_back(std::move(desktop));
  proc::proc_t target(std::move(env), std::move(apps));
  const auto provider = proc::app_process_identity_provider(target);

  EXPECT_EQ(provider().process_id, 0u);
  EXPECT_EQ(provider().creation_time, 0u);
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
  ASSERT_EQ(target.execute(7, launch_session), 0);
  EXPECT_NE(provider().process_id, 0u);
#ifdef _WIN32
  // The marker is queried from the owned child handle, so it is verifiable for a live child.
  EXPECT_NE(provider().creation_time, 0u);
  // Exact-match oracle: the marker must equal an independent OS query, so a
  // constant-but-wrong marker fails.
  {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, provider().process_id);
    ASSERT_NE(handle, nullptr);
    FILETIME creation {};
    FILETIME exit_time {};
    FILETIME kernel_time {};
    FILETIME user_time {};
    ASSERT_TRUE(GetProcessTimes(handle, &creation, &exit_time, &kernel_time, &user_time));
    CloseHandle(handle);
    const auto queried_creation_time = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | static_cast<std::uint64_t>(creation.dwLowDateTime);
    EXPECT_NE(queried_creation_time, 0u);
    EXPECT_EQ(provider().creation_time, queried_creation_time);
  }
#else
  // POSIX reports a zero marker, so the EXPECT below requires creation_time 0u
  // (unverifiable by contract, not by backend prediction).
  EXPECT_EQ(provider().creation_time, 0u);
#endif
  EXPECT_EQ(provider().process_id, target.app_process_id());

  target.terminate();
  EXPECT_EQ(provider().process_id, 0u);
  EXPECT_EQ(provider().creation_time, 0u);

  // The Desktop placebo session reports {0, 0} on every platform.
  ASSERT_EQ(target.execute(42, launch_session), 0);
  EXPECT_EQ(provider().process_id, 0u);
  EXPECT_EQ(provider().creation_time, 0u);
  target.terminate();
  EXPECT_EQ(provider().process_id, 0u);
  EXPECT_EQ(provider().creation_time, 0u);
}

#ifndef _WIN32

namespace {
  /**
   * @brief Scratch directory for file-gate concurrency tests, removed on destruction.
   */
  struct gate_dir_t {
    fs::path root;  ///< Scratch root; gate, setting, and script files live here.

    /**
     * @brief Create an empty scratch directory for one test.
     *
     * @param name Test-unique directory infix.
     */
    explicit gate_dir_t(const char *name) {
      root = fs::temp_directory_path() / std::format("sunshine-proc-{}-{}", name, ::getpid());
      std::error_code ec;
      fs::remove_all(root, ec);
      fs::create_directories(root, ec);
    }

    ~gate_dir_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };

  /**
   * @brief Write an executable `sh` script for a prep/undo command.
   *
   * Prep commands run without a shell, so file writes and gate waits are
   * staged as scripts instead of `sh -c` one-liners with quotes.
   *
   * @param path Script file to write.
   * @param body Script body after the shebang line.
   */
  void write_gate_script(const fs::path &path, const std::string &body) {
    std::ofstream {path} << "#!/bin/sh\n"
                         << body;
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
  }

  /**
   * @brief Poll for a file-gate signal with a bounded wait.
   *
   * @param path Signal file to wait for.
   * @param attempts Number of 10ms polls before giving up.
   * @return True when the file appeared before the deadline.
   */
  bool wait_for_gate_file(const fs::path &path, int attempts = 500) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
      std::error_code ec;
      if (fs::exists(path, ec) && !ec) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  /**
   * @brief Read a small text file, stripping trailing newlines.
   *
   * @param path File to read.
   * @return Trimmed file content, or empty when the file is missing.
   */
  std::string read_trimmed_gate_file(const fs::path &path) {
    std::ifstream input {path};
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
      content.pop_back();
    }
    return content;
  }

  /**
   * @brief Per-app setup for a gated-launch scenario.
   */
  struct gated_app_config_t {
    std::string name;  ///< Application name.
    std::string id;  ///< Application id.
    std::string cmd;  ///< Command line to launch.
    std::string do_marker;  ///< Marker the do script writes to the setting file.
    bool do_gated = false;  ///< Do script signals start, then waits on the gate.
    std::string undo_marker;  ///< Marker the undo script writes; empty means no undo script.
    bool undo_gated = false;  ///< Undo script signals start, then waits on the gate.
  };

  /**
   * @brief Owns the files, scripts, and process manager for one gated-launch test.
   *
   * The gated lifecycle tests share the same setup: a scratch directory, marker
   * scripts, app contexts, and a process manager with a launch session. The
   * fixture also carries the background-launch thread and releases the gate on
   * destruction, so each test body keeps only its orchestration and assertions.
   */
  struct gated_launch_t {
    gate_dir_t dir;  ///< Scratch root; created first, removed last.
    fs::path setting_file;  ///< Marker file the scripts write.
    fs::path gate_file;  ///< Gate file; creating it releases gated scripts.
    fs::path signal_file;  ///< Signal file gated scripts touch on entry.
    std::shared_ptr<rtsp_stream::launch_session_t> session;  ///< Session for execute().
    proc::proc_t target;  ///< Process manager under test.
    std::atomic<int> exec_result {-999};  ///< Background execute() result.
    std::jthread exec_thread;  ///< Background execute() thread, joined explicitly.

    /**
     * @brief Build the scenario: directories, scripts, apps, and process manager.
     *
     * @param tag Test-unique directory infix.
     * @param configs One entry per app under test.
     */
    gated_launch_t(const char *tag, std::vector<gated_app_config_t> configs);

    /**
     * @brief Terminate the target and release the gate unless disarmed.
     *
     * Replaces the per-test terminate/release guards: firing here covers both
     * normal completion and early assertion exits.
     */
    ~gated_launch_t();

    /**
     * @brief Run execute() for one app on the background thread.
     *
     * @param id App id to launch; the result lands in exec_result.
     */
    void launch_in_background(int id);

    /**
     * @brief Poll running() with a deadline, releasing the gate on timeout.
     *
     * @return Poll result, or nullopt when the deadline expired.
     */
    std::optional<int> poll_running_or_release_gate();

    /**
     * @brief Create the gate file, reporting success for assertion.
     *
     * @return True when the file opened for writing.
     */
    bool open_gate_file();

    /**
     * @brief Disarm the destructor's gate release after an explicit release.
     */
    void disarm_gate_release();

  private:
    bool gate_armed_ = true;  ///< Destructor releases the gate unless disarmed.

    /**
     * @brief Build app contexts with prep commands wired to the scenario scripts.
     *
     * @param root Scratch root holding the scripts.
     * @param configs One entry per app under test.
     * @return App list for proc_t construction.
     */
    static std::vector<proc::ctx_t> build_apps(const fs::path &root, const std::vector<gated_app_config_t> &configs);

    /**
     * @brief Write each app's marker scripts.
     *
     * @param configs One entry per app under test.
     */
    void write_scripts(const std::vector<gated_app_config_t> &configs);

    /**
     * @brief Render one marker-script body.
     *
     * @param marker Marker written to the setting file.
     * @param gated Signal start, then wait on the gate before writing.
     * @return Script body after the shebang line.
     */
    std::string marker_script_body(const std::string &marker, bool gated);
  };

  gated_launch_t::gated_launch_t(const char *tag, std::vector<gated_app_config_t> configs):
      dir {tag},
      setting_file {dir.root / "setting"},
      gate_file {dir.root / "gate"},
      signal_file {dir.root / "started"},
      session {std::make_shared<rtsp_stream::launch_session_t>()},
      target {boost::this_process::environment(), build_apps(dir.root, configs)} {
    write_scripts(configs);
  }

  gated_launch_t::~gated_launch_t() {
    target.terminate();
    if (gate_armed_) {
      std::ofstream {gate_file};
    }
  }

  void gated_launch_t::launch_in_background(int id) {
    exec_thread = std::jthread {[this, id]() {
      exec_result.store(target.execute(id, session));
    }};
  }

  std::optional<int> gated_launch_t::poll_running_or_release_gate() {
    auto poll = std::async(std::launch::async, [this]() {
      return target.running();
    });
    if (poll.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
      std::ofstream {gate_file};
      gate_armed_ = false;
      return std::nullopt;
    }
    return poll.get();
  }

  bool gated_launch_t::open_gate_file() {
    std::ofstream gate {gate_file};
    return static_cast<bool>(gate);
  }

  void gated_launch_t::disarm_gate_release() {
    gate_armed_ = false;
  }

  std::vector<proc::ctx_t> gated_launch_t::build_apps(const fs::path &root, const std::vector<gated_app_config_t> &configs) {
    std::vector<proc::ctx_t> apps;
    for (const auto &config : configs) {
      proc::ctx_t ctx {};
      ctx.name = config.name;
      ctx.id = config.id;
      ctx.cmd = config.cmd;
      std::string undo_path;
      if (!config.undo_marker.empty()) {
        undo_path = (root / std::format("undo_{}.sh", config.id)).string();
      }
      ctx.prep_cmds.emplace_back((root / std::format("do_{}.sh", config.id)).string(), std::move(undo_path), false);
      apps.push_back(std::move(ctx));
    }
    return apps;
  }

  void gated_launch_t::write_scripts(const std::vector<gated_app_config_t> &configs) {
    for (const auto &config : configs) {
      write_gate_script(dir.root / std::format("do_{}.sh", config.id), marker_script_body(config.do_marker, config.do_gated));
      if (!config.undo_marker.empty()) {
        write_gate_script(dir.root / std::format("undo_{}.sh", config.id), marker_script_body(config.undo_marker, config.undo_gated));
      }
    }
  }

  std::string gated_launch_t::marker_script_body(const std::string &marker, bool gated) {
    if (!gated) {
      return std::format("echo {} > \"{}\"\n", marker, setting_file.string());
    }
    return std::format(
      "touch \"{}\"\nwhile [ ! -f \"{}\" ]; do sleep 0.01; done\necho {} > \"{}\"\n",
      signal_file.string(),
      gate_file.string(),
      marker,
      setting_file.string()
    );
  }

}  // namespace

/**
 * @brief Verify running() cleanup after a relaunch never terminates the successor app.
 */
TEST(ProcessTest, RunningAfterRelaunchKeepsSuccessorAlive) {
  boost::process::v1::environment env = boost::this_process::environment();
  std::vector<proc::ctx_t> apps;
  for (const auto id : {"7", "8"}) {
    proc::ctx_t ctx {};
    ctx.name = "Sleeper";
    ctx.id = id;
    ctx.cmd = "sleep 30";
    apps.push_back(std::move(ctx));
  }
  proc::proc_t target(std::move(env), std::move(apps));
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

  ASSERT_EQ(target.execute(7, launch_session), 0);
  const auto first_pid = target.app_process_id();
  ASSERT_NE(first_pid, 0u);
  target.terminate();
  EXPECT_EQ(target.running(), 0);

  ASSERT_EQ(target.execute(8, launch_session), 0);
  const auto successor_pid = target.app_process_id();
  ASSERT_NE(successor_pid, 0u);
  EXPECT_EQ(target.running(), 8);
  EXPECT_EQ(target.running(), 8);
  EXPECT_EQ(target.app_process_id(), successor_pid);

  target.terminate();
  EXPECT_EQ(target.running(), 0);
}

/**
 * @brief Verify a launch overlapping prior cleanup waits for that cleanup to finish.
 *
 * Blocks A's undo on a file gate, attempts B's launch concurrently, releases A,
 * and verifies B's prep setting survives. Without the transition gate, B's prep
 * runs while A's termination is still blocked, and A's undo then overwrites B's
 * setting after B starts. Display revert runs in the same finish_termination()
 * critical section as the undo, so the file ordering also pins that no old
 * display-revert runs after B starts. Sequential relaunch coverage alone cannot
 * catch this: it never overlaps cleanup with the next launch.
 */
TEST(ProcessTest, OverlappingLaunchWaitsForPriorCleanup) {
  gated_launch_t launch {"overlap", {
                                      {"AppA", "7", "sleep 30", "A-prep", false, "A-undo", true},
                                      {"AppB", "8", "sleep 30", "B-prep"},
                                    }};
  ASSERT_TRUE(fs::exists(launch.dir.root));

  ASSERT_EQ(launch.target.execute(7, launch.session), 0);
  ASSERT_NE(launch.target.app_process_id(), 0u);

  ASSERT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep");

  // Terminate A in the background: its undo signals start, then blocks on the gate.
  std::jthread terminate_thread {[&launch]() {
    launch.target.terminate();
  }};
  ASSERT_TRUE(wait_for_gate_file(launch.signal_file)) << "A's undo did not start; cannot overlap B's launch";

  // Attempt B's launch while A's cleanup is blocked. The transition gate must hold it.
  std::atomic<bool> b_attempting {false};
  std::atomic<bool> b_launch_done {false};
  std::atomic<int> b_launch_result {-999};
  std::latch b_started {1};
  std::jthread b_thread {[&launch, &b_attempting, &b_launch_done, &b_launch_result, &b_started]() {
    b_attempting.store(true);
    b_started.count_down();
    const int result = launch.target.execute(8, launch.session);
    b_launch_result.store(result);
    b_launch_done.store(true);
  }};
  b_started.wait();
  ASSERT_TRUE(b_attempting.load());
  // Give B a chance to (incorrectly) run its prep while A is still blocked.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_FALSE(b_launch_done.load()) << "B's launch finished while A's cleanup was still blocked";
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep") << "B's prep ran before A's undo finished";

  // Release A's undo; both threads must then complete with B's setting intact.
  ASSERT_TRUE(launch.open_gate_file()) << "failed to release the undo gate";
  terminate_thread.join();
  b_thread.join();
  launch.disarm_gate_release();
  EXPECT_EQ(b_launch_result.load(), 0);
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "B-prep") << "A's undo overwrote B's prep after B started";
  EXPECT_EQ(launch.target.running(), 8);
  EXPECT_NE(launch.target.app_process_id(), 0u);
}

/**
 * @brief Verify `running()` reaps exited children on the Desktop placebo poll.
 *
 * The placebo report in `proc_t::running()` must still run the
 * POSIX `waitpid()` reaper; otherwise exited detached children linger as
 * zombies for the whole Desktop session.
 */
TEST(ProcessTest, RunningReapsExitedChildOnPlaceboPoll) {
  boost::process::v1::environment env = boost::this_process::environment();
  proc::ctx_t ctx {};
  ctx.name = "Desktop";
  ctx.id = "42";
  std::vector<proc::ctx_t> apps;
  apps.push_back(std::move(ctx));
  proc::proc_t target(std::move(env), std::move(apps));
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
  ASSERT_EQ(target.execute(42, launch_session), 0);
  const auto cleanup = util::fail_guard([&target]() {
    target.terminate();
  });
  ASSERT_EQ(target.running(), 42);

  const pid_t child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    _exit(0);
  }
  const auto reap_sacrificial = util::fail_guard([child]() {
    // Test-local cleanup only; never masks the assertion below.
    while (waitpid(child, nullptr, WNOHANG) > 0);
  });

  // Wait until the sacrificial child is an unreaped zombie, peeking with
  // `waitid()`/`WNOWAIT` so the test itself never reaps it.
  bool is_zombie = false;
  for (int attempt = 0; attempt < 1000 && !is_zombie; ++attempt) {
    siginfo_t info {};
    if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == child) {
      is_zombie = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ASSERT_TRUE(is_zombie) << "sacrificial child did not become a zombie";

  EXPECT_EQ(target.running(), 42);

  siginfo_t info {};
  errno = 0;
  EXPECT_EQ(waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT), -1);
  EXPECT_EQ(errno, ECHILD) << "running() left the exited child as a zombie";
}

/**
 * @brief Verify running() answers promptly while execute() is blocked in a prep wait.
 *
 * The prep script signals start, then waits on a file gate; execute() runs on
 * a background thread holding the transition gate. running() must report the
 * in-flight app id (launch-busy) within the deadline instead of blocking on
 * the gate. Fails on the pre-fix blocking running(), which waits for release.
 */
TEST(ProcessTest, RunningReportsLaunchInFlightDuringGatedPrep) {
  gated_launch_t launch {"t1-exec-gate", {{"Gated", "7", "sleep 30", "G-prep", true}}};
  ASSERT_TRUE(fs::exists(launch.dir.root));

  launch.launch_in_background(7);
  ASSERT_TRUE(wait_for_gate_file(launch.signal_file)) << "gated prep did not start; cannot overlap the poll";

  const auto poll_result = launch.poll_running_or_release_gate();
  ASSERT_TRUE(poll_result.has_value()) << "running() blocked on execute()'s prep wait";
  EXPECT_EQ(*poll_result, 7);
  ASSERT_TRUE(launch.open_gate_file()) << "failed to release the prep gate";
  launch.exec_thread.join();
  launch.disarm_gate_release();
  EXPECT_EQ(launch.exec_result.load(), 0);
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "G-prep");
  EXPECT_EQ(launch.target.running(), 7);
}

/**
 * @brief Verify running() answers promptly while terminate() is blocked in an undo wait.
 *
 * terminate() claims first (publishing _app_id -1), then blocks on the gated
 * undo holding the transition gate. running() must report 0 from the
 * post-claim fast read within the deadline. Fails on the pre-fix blocking
 * running(), which waits for release.
 */
TEST(ProcessTest, RunningReportsZeroDuringGatedUndo) {
  gated_launch_t launch {"t2-term-gate", {{"Gated", "7", "sleep 30", "A-prep", false, "A-undo", true}}};
  ASSERT_TRUE(fs::exists(launch.dir.root));
  ASSERT_EQ(launch.target.execute(7, launch.session), 0);
  ASSERT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep");

  std::jthread terminate_thread {[&launch]() {
    launch.target.terminate();
  }};
  ASSERT_TRUE(wait_for_gate_file(launch.signal_file)) << "gated undo did not start; cannot overlap the poll";

  const auto poll_result = launch.poll_running_or_release_gate();
  ASSERT_TRUE(poll_result.has_value()) << "running() blocked on terminate()'s undo wait";
  EXPECT_EQ(*poll_result, 0);
  ASSERT_TRUE(launch.open_gate_file()) << "failed to release the undo gate";
  terminate_thread.join();
  launch.disarm_gate_release();
  EXPECT_EQ(launch.target.running(), 0);
  EXPECT_EQ(launch.target.app_process_id(), 0u);
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "A-undo");
}

/**
 * @brief Verify running()-driven cleanup still orders a concurrent launch after the undo.
 *
 * A exits naturally (detected via app_process_id() only, so the corpse stays
 * unclaimed); a background running() claims it and blocks in the gated undo
 * holding the transition gate. B's concurrent launch must wait: its prep must
 * not run while the gate is closed, and after release B's setting wins. A
 * second poll during the owned cleanup answers promptly with the post-claim 0
 * instead of blocking, which is what distinguishes the try-gated design from a
 * blocking cleanup gate. Fails pre-fix: the first running() either blocks
 * nothing (no ordering without the gate) or, under a blocking gate, the
 * second poll stalls past the deadline.
 */
TEST(ProcessTest, RunningCleanupPreservesLaunchOrdering) {
  gated_launch_t launch {"t3-cleanup-order", {
                                               {"AppA", "7", "true", "A-prep", false, "A-undo", true},
                                               {"AppB", "8", "sleep 30", "B-prep"},
                                             }};
  ASSERT_TRUE(fs::exists(launch.dir.root));
  ASSERT_EQ(launch.target.execute(7, launch.session), 0);
  ASSERT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep");

  // Detect the natural exit via the identity getter only: any running() call
  // here would claim the cleanup this test overlaps with B's launch.
  bool exited = false;
  for (int attempt = 0; attempt < 500 && !exited; ++attempt) {
    if (launch.target.app_process_id() == 0u) {
      exited = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_TRUE(exited) << "A did not exit naturally";

  std::atomic<int> cleanup_result {-999};
  std::jthread cleanup_thread {[&launch, &cleanup_result]() {
    cleanup_result.store(launch.target.running());
  }};
  ASSERT_TRUE(wait_for_gate_file(launch.signal_file)) << "cleanup undo did not start; cannot overlap B's launch";

  std::atomic<bool> b_launch_done {false};
  std::atomic<int> b_launch_result {-999};
  std::jthread b_thread {[&launch, &b_launch_done, &b_launch_result]() {
    const int result = launch.target.execute(8, launch.session);
    b_launch_result.store(result);
    b_launch_done.store(true);
  }};
  // Give B a chance to (incorrectly) run its prep while A's cleanup is still blocked.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_FALSE(b_launch_done.load()) << "B's launch finished while A's cleanup was still blocked";
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep") << "B's prep ran before A's undo finished";

  // A second poll while the cleanup owns the gate answers promptly instead of
  // blocking on it; the corpse is already claimed, so it reports 0.
  const auto poll_result = launch.poll_running_or_release_gate();
  ASSERT_TRUE(poll_result.has_value()) << "second running() blocked on the owned cleanup";
  EXPECT_EQ(*poll_result, 0);
  ASSERT_TRUE(launch.open_gate_file()) << "failed to release the undo gate";
  cleanup_thread.join();
  b_thread.join();
  launch.disarm_gate_release();
  EXPECT_EQ(cleanup_result.load(), 0);
  EXPECT_EQ(b_launch_result.load(), 0);
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "B-prep") << "A's undo overwrote B's prep after B started";
  EXPECT_EQ(launch.target.running(), 8);
}

/**
 * @brief Hammer running() across steady run, natural exit, and racing terminate.
 *
 * Poller threads report only {0, 7} while the main thread drives a natural
 * exit and then a racing terminate; the test passing at all (cleanly, under
 * ASan/TSan) pins the single-hold reaper/claim atomicity. Guard-only: it
 * asserts no promptness or ordering, which the gated tests above pin.
 */
TEST(ProcessTest, RunningConcurrentPollsStayConsistentDuringExitAndTerminate) {
  boost::process::v1::environment env = boost::this_process::environment();
  proc::ctx_t ctx {};
  ctx.name = "Sleeper";
  ctx.id = "7";
  ctx.cmd = "sleep 30";
  std::vector<proc::ctx_t> apps;
  apps.push_back(std::move(ctx));
  proc::proc_t target(std::move(env), std::move(apps));
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
  const auto terminate_target = util::fail_guard([&target]() {
    target.terminate();
  });

  std::atomic<bool> saw_bad_id {false};
  const auto hammer = [&target, &saw_bad_id](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      const int id = target.running();
      if (id != 0 && id != 7) {
        saw_bad_id.store(true);
      }
      (void) target.app_process_id();
    }
  };

  // Phase 1: steady run, then a natural exit raced with the pollers.
  ASSERT_EQ(target.execute(7, launch_session), 0);
  {
    std::vector<std::jthread> pollers;
    for (int i = 0; i < 4; ++i) {
      pollers.emplace_back(hammer);
    }
    const auto stop_pollers = util::fail_guard([&pollers]() {
      for (auto &poller : pollers) {
        poller.request_stop();
      }
    });
    const auto pid = target.app_process_id();
    ASSERT_NE(pid, 0u);
    ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGKILL), 0);
    bool cleaned = false;
    for (int attempt = 0; attempt < 500 && !cleaned; ++attempt) {
      if (target.running() == 0) {
        cleaned = true;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    EXPECT_TRUE(cleaned) << "natural exit was never cleaned up under the hammer";
  }

  // Phase 2: steady run, then a racing terminate.
  ASSERT_EQ(target.execute(7, launch_session), 0);
  {
    std::vector<std::jthread> pollers;
    for (int i = 0; i < 4; ++i) {
      pollers.emplace_back(hammer);
    }
    const auto stop_pollers = util::fail_guard([&pollers]() {
      for (auto &poller : pollers) {
        poller.request_stop();
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    target.terminate();
    EXPECT_EQ(target.running(), 0);
    EXPECT_EQ(target.app_process_id(), 0u);
  }

  EXPECT_FALSE(saw_bad_id.load()) << "poller observed an id outside {0, 7}";
}

/**
 * @brief Verify terminate() during a gated launch waits, then drains once released.
 *
 * execute() holds the transition gate across the prep wait, so terminate()
 * cannot interleave: it blocks until the gate releases, execute() reports 0,
 * and terminate() then undoes the completed prep. The final state stays
 * consistent: terminal reports are 0 and a successor launch works.
 */
TEST(ProcessTest, TerminateDuringLaunchDrainsAfterGateRelease) {
  gated_launch_t launch {"b4-term-launch", {{"Gated", "7", "sleep 30", "L-prep", true, "L-undo", false}}};
  ASSERT_TRUE(fs::exists(launch.dir.root));

  launch.launch_in_background(7);
  ASSERT_TRUE(wait_for_gate_file(launch.signal_file)) << "gated prep did not start; cannot overlap terminate()";

  // Launch in flight: pollers fail closed with the new id while prep is gated.
  EXPECT_EQ(launch.target.running(), 7);

  // terminate() blocks on execute()'s transition until the releaser opens the gate.
  std::jthread releaser {[&launch]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::ofstream {launch.gate_file};
  }};
  launch.target.terminate();
  releaser.join();
  launch.exec_thread.join();
  launch.disarm_gate_release();

  EXPECT_EQ(launch.exec_result.load(), 0);
  EXPECT_EQ(launch.target.running(), 0);
  EXPECT_EQ(launch.target.app_process_id(), 0u);
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "L-undo") << "terminate did not undo the completed prep";

  // A successor launch works on the consistent state.
  ASSERT_EQ(launch.target.execute(7, launch.session), 0);
  EXPECT_EQ(launch.target.running(), 7);
}

/**
 * @brief Verify an OS-killed app is cleaned up by running(), with undo and successor intact.
 *
 * After SIGKILL, the death is detected via app_process_id() only, so the
 * corpse stays unclaimed until the single running() call triggers the cleanup:
 * the undo must not have run before it, and must have run after it. A
 * successor launch then works. Pins the natural-death path distinctly from
 * the terminate() path.
 */
TEST(ProcessTest, NaturalDeathCleansUpViaRunning) {
  gated_launch_t launch {"b4-os-death", {{"Sleeper", "7", "sleep 30", "A-prep", false, "A-undo", false}}};
  ASSERT_TRUE(fs::exists(launch.dir.root));
  ASSERT_EQ(launch.target.execute(7, launch.session), 0);
  ASSERT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep");
  const auto pid = launch.target.app_process_id();
  ASSERT_NE(pid, 0u);
  ASSERT_EQ(::kill(static_cast<pid_t>(pid), 0), 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGKILL), 0);

  bool dead = false;
  for (int attempt = 0; attempt < 500 && !dead; ++attempt) {
    if (launch.target.app_process_id() == 0u) {
      dead = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_TRUE(dead) << "killed app was never observed dead";
  ASSERT_EQ(read_trimmed_gate_file(launch.setting_file), "A-prep") << "undo ran before running() claimed the corpse";

  EXPECT_EQ(launch.target.running(), 0);
  EXPECT_EQ(launch.target.app_process_id(), 0u);
  EXPECT_EQ(read_trimmed_gate_file(launch.setting_file), "A-undo") << "running() cleanup did not run the undo";

  // A successor launch works on the consistent state.
  ASSERT_EQ(launch.target.execute(7, launch.session), 0);
  EXPECT_EQ(launch.target.running(), 7);
}
#endif

TEST_F(ProcessUpdateTest, UpdateAppsAndEnv_PreservesRunningState) {
  proc::proc.update_apps_and_env(buildNewSource());

  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "NewApp");
  EXPECT_EQ(proc::proc.running(), 42) << "update_apps_and_env must not reset _app_id or placebo";
}

TEST_F(ProcessUpdateTest, UpdateAppsAndEnv_PreservesSessionEnvironment) {
  proc::proc.update_apps_and_env(buildNewSource());

  const auto &active_env = proc::proc.get_env();
  EXPECT_TRUE(active_env.count("SUNSHINE_APP_NAME") > 0)
    << "SUNSHINE_APP_NAME was wiped from the environment during update_apps_and_env";

  if (active_env.count("SUNSHINE_APP_NAME") > 0) {
    EXPECT_EQ(active_env.at("SUNSHINE_APP_NAME").to_string(), "Desktop");
  }
}

TEST_F(ProcessRefreshTest, Refresh_ParsesFileOnFirstCall) {
  // The first call to refresh() must always parse the file, regardless
  // of what file_time_type{} compares to relative to the file timestamp.
  //
  // NOTE: Because refresh() uses static local variables (has_parsed, last_apps_file_update),
  // this test exercises the very first call in this test process.  Subsequent
  // tests that call refresh() share the same static state.
  const fs::path apps_file = test_dir / "apps_initial.json";
  writeAppsJson(apps_file, {"TestApp1", "TestApp2"});

  // Save and clear global proc
  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  ASSERT_TRUE(proc::proc.get_apps().empty());

  // Act
  proc::refresh(apps_file.string());

  // Assert — must have been parsed
  ASSERT_EQ(proc::proc.get_apps().size(), 2u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "TestApp1");
  EXPECT_EQ(proc::proc.get_apps()[1].name, "TestApp2");

  // Cleanup
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_SkipsUnchangedFile) {
  const fs::path apps_file = test_dir / "apps_skip.json";
  writeAppsJson(apps_file, {"OriginalApp"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  // First refresh — should parse
  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);

  // Mutate apps in-memory to detect whether the second refresh re-parses
  proc::proc.get_apps()[0].name = "Mutated";

  // Second refresh with unchanged file — should be skipped
  proc::refresh(apps_file.string());
  EXPECT_EQ(proc::proc.get_apps()[0].name, "Mutated")
    << "refresh() should skip re-parse when the file timestamp has not changed";

  // Cleanup
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_ReparseAfterFileModified) {
  const fs::path apps_file = test_dir / "apps_modified.json";
  writeAppsJson(apps_file, {"BeforeEdit"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "BeforeEdit");

  // Rewrite to ensure a new timestamp
  writeAppsJson(apps_file, {"AfterEdit"});

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "AfterEdit");

  // Cleanup
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_ReparseAfterFileMtimeDecreased) {
  const fs::path apps_file = test_dir / "apps_reverted.json";
  writeAppsJson(apps_file, {"Initial"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "Initial");

  // Write a new file but artificially set its timestamp to the PAST
  // simulating a backup restoration or git checkout
  writeAppsJson(apps_file, {"OlderBackup"});
  auto past_time = fs::file_time_type::clock::now() - std::chrono::hours(1);
  fs::last_write_time(apps_file, past_time);

  proc::refresh(apps_file.string());
  ASSERT_EQ(proc::proc.get_apps().size(), 1u);
  EXPECT_EQ(proc::proc.get_apps()[0].name, "OlderBackup")
    << "refresh() must re-parse if the modification time differs, even if it is strictly earlier";

  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, Refresh_PreservesRunningAppDuringReparse) {
  // This is the critical regression test: refreshing the app list while an
  // app is running must not lose the active process state.
  const fs::path apps_file = test_dir / "apps_running.json";
  writeAppsJson(apps_file, {"Desktop"});

  auto saved = std::move(proc::proc);
  proc::proc = proc::proc_t {};

  // Initial parse
  proc::refresh(apps_file.string());
  ASSERT_FALSE(proc::proc.get_apps().empty());

  // Find the ID of "Desktop" as assigned by parse
  auto desktop_id = std::stoi(proc::proc.get_apps()[0].id);

  // Execute it in placebo mode (empty cmd)
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
  launch_session->width = 1920;
  launch_session->height = 1080;
  launch_session->fps = 60;
  launch_session->gcmap = 0;
  launch_session->enable_hdr = false;
  launch_session->host_audio = false;
  launch_session->enable_sops = false;
  launch_session->surround_info = 2;
  int rc = proc::proc.execute(desktop_id, launch_session);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(proc::proc.running(), desktop_id);

  // Simulate an external edit to the apps file
  writeAppsJson(apps_file, {"Desktop", "NewApp"});

  // Act — refresh while the app is "running"
  proc::refresh(apps_file.string());

  // Assert — app list is updated but the running state is preserved
  ASSERT_EQ(proc::proc.get_apps().size(), 2u);
  EXPECT_EQ(proc::proc.running(), desktop_id)
    << "refresh() during a running session must preserve active process state";

  // Cleanup — terminate before restoring
  proc::proc.terminate();
  proc::proc = std::move(saved);
}

TEST_F(ProcessRefreshTest, FileTimeType_DefaultValueComparison) {
  // This test documents the behavior that triggered the timestamp guard
  // regression. On some toolchains (MSYS2 UCRT64 GCC 16.2), file timestamps
  // and clock::now() can compare less-or-equal to file_time_type{}.
  //
  // The fix uses std::optional<file_time_type> instead of relying on
  // file_time_type{} as a "never parsed" indicator.
  const auto default_time = std::filesystem::file_time_type {};
  const auto now_time = std::filesystem::file_time_type::clock::now();

  // We cannot assert the comparison direction since it is platform-dependent.
  // Instead, just log and document both cases.
  if (now_time <= default_time) {
    BOOST_LOG(info) << "file_time_type::clock::now() <= file_time_type{} is TRUE on this platform "
                    << "(this is the UCRT64 case that requires the optional sentinel)";
  } else {
    BOOST_LOG(info) << "file_time_type::clock::now() > file_time_type{} on this platform "
                    << "(default-init guard would have worked, but optional is still safer)";
  }

  // The actual correctness is verified by Refresh_ParsesFileOnFirstCall above —
  // if that test passes, the optional fix works regardless of platform behavior.
  SUCCEED();
}
