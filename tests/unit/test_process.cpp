/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <system_error>
#include <thread>

// lib includes
#include <boost/process/v1.hpp>

#ifdef _WIN32
  // lib includes
  #include <boost/process/v1/windows.hpp>
  #include <Windows.h>
#else
  // lib includes
  #include <unistd.h>
#endif

// local includes
#include <src/config.h>
#include <src/nvhttp.h>
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

/**
 * @brief Test fixture for the synthetic "Input Only" app entry.
 */
class ProcessInputOnlyTest: public ProcessRefreshTest {  // NOSONAR(cpp:S3656): protected members are intentional for test fixture subclassing
protected:
  bool saved_enable_input_only_mode;  ///< Input-only mode setting restored after each test.
  std::string saved_input_only_app_id;  ///< Cached placeholder app id restored after each test.

  void SetUp() override {
    ProcessRefreshTest::SetUp();
    saved_enable_input_only_mode = config::input.enable_input_only_mode;
    saved_input_only_app_id = proc::input_only_app_id;
  }

  void TearDown() override {
    config::input.enable_input_only_mode = saved_enable_input_only_mode;
    proc::input_only_app_id = saved_input_only_app_id;
    ProcessRefreshTest::TearDown();
  }

  /**
   * @brief Build a context matching the synthetic "Input Only" placeholder.
   *
   * @param id Identifier of the placeholder entry.
   * @return Context that launches no command and streams no captured video.
   */
  static proc::ctx_t make_placeholder_ctx(const std::string &id) {
    proc::ctx_t ctx;
    ctx.name = "Input Only";
    ctx.id = id;
    ctx.auto_detach = true;
    ctx.wait_all = true;
    ctx.exit_timeout = std::chrono::seconds {5};
    ctx.input_only = true;
    return ctx;
  }

  /**
   * @brief Build a context matching an ordinary application.
   *
   * @param id Identifier of the application entry.
   * @return Context that launches no command and captures video normally.
   */
  static proc::ctx_t make_regular_ctx(const std::string &id) {
    proc::ctx_t ctx;
    ctx.name = "Desktop";
    ctx.id = id;
    return ctx;
  }

  /**
   * @brief Create a process manager owning only the supplied application contexts.
   *
   * @param apps Application contexts owned by the manager.
   * @return Process manager using the current environment and the supplied apps.
   */
  static proc::proc_t make_process(std::vector<proc::ctx_t> apps) {
    boost::process::v1::environment env = boost::this_process::environment();
    return proc::proc_t {std::move(env), std::move(apps)};
  }
};

TEST_F(ProcessInputOnlyTest, Parse_AddsInputOnlyEntryWhenEnabled) {
  config::input.enable_input_only_mode = true;
  proc::input_only_app_id.clear();

  const auto path = test_dir / "apps_input_only_enabled.json";
  writeAppsJson(path, {"Game One", "Game Two"});

  auto parsed = proc::parse(path.string());
  ASSERT_TRUE(parsed.has_value());

  const auto &apps = parsed->get_apps();
  ASSERT_EQ(apps.size(), 3U);

  const auto &entry = apps.back();
  EXPECT_EQ(entry.name, "Input Only");
  EXPECT_TRUE(entry.cmd.empty());
  EXPECT_FALSE(entry.id.empty());
  EXPECT_TRUE(entry.input_only);
  EXPECT_EQ(proc::input_only_app_id, entry.id);

  // Existing apps keep stable IDs regardless of the input-only flag
  EXPECT_EQ(apps[0].name, "Game One");
  EXPECT_FALSE(apps[0].input_only);
  EXPECT_EQ(apps[1].name, "Game Two");
  EXPECT_FALSE(apps[1].input_only);
}

TEST_F(ProcessInputOnlyTest, Parse_OmitsInputOnlyEntryWhenDisabled) {
  config::input.enable_input_only_mode = false;
  proc::input_only_app_id = "stale-id";

  const auto path = test_dir / "apps_input_only_disabled.json";
  writeAppsJson(path, {"Game One", "Game Two"});

  auto parsed = proc::parse(path.string());
  ASSERT_TRUE(parsed.has_value());

  const auto &apps = parsed->get_apps();
  ASSERT_EQ(apps.size(), 2U);
  for (const auto &app : apps) {
    EXPECT_NE(app.name, "Input Only");
  }
  EXPECT_TRUE(proc::input_only_app_id.empty());
}

TEST_F(ProcessInputOnlyTest, Parse_KeepsExistingAppIdsStableWhenToggled) {
  const auto path = test_dir / "apps_input_only_toggle.json";
  writeAppsJson(path, {"Game One", "Game Two"});

  config::input.enable_input_only_mode = false;
  auto disabled = proc::parse(path.string());
  ASSERT_TRUE(disabled.has_value());

  config::input.enable_input_only_mode = true;
  auto enabled = proc::parse(path.string());
  ASSERT_TRUE(enabled.has_value());

  const auto &disabled_apps = disabled->get_apps();
  const auto &enabled_apps = enabled->get_apps();
  ASSERT_EQ(enabled_apps.size(), disabled_apps.size() + 1);
  for (size_t i = 0; i < disabled_apps.size(); ++i) {
    EXPECT_EQ(enabled_apps[i].id, disabled_apps[i].id);
  }
}

TEST_F(ProcessInputOnlyTest, IsInputOnlyRunning_FollowsPlaceholderLifetime) {
  auto process = make_process({make_placeholder_ctx("777")});

  EXPECT_FALSE(process.is_input_only_running());

  ASSERT_EQ(process.execute(777, std::make_shared<rtsp_stream::launch_session_t>()), 0);

  // The placeholder launches no command, so it stays running via the placebo flag
  EXPECT_EQ(process.running(), 777);
  EXPECT_TRUE(process.is_input_only_running());

  process.terminate();

  EXPECT_EQ(process.running(), 0);
  EXPECT_FALSE(process.is_input_only_running());
  EXPECT_TRUE(process.get_last_run_app_name().empty());
}

TEST_F(ProcessInputOnlyTest, IsInputOnlyRunning_FalseForRegularApp) {
  auto process = make_process({make_regular_ctx("778")});

  ASSERT_EQ(process.execute(778, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  EXPECT_EQ(process.running(), 778);
  EXPECT_FALSE(process.is_input_only_running());

  process.terminate();
  EXPECT_FALSE(process.is_input_only_running());
}

TEST_F(ProcessInputOnlyTest, IsInputOnlyRunning_IgnoredWhenModeToggledOffMidSession) {
  auto process = make_process({make_placeholder_ctx("779")});
  ASSERT_EQ(process.execute(779, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  ASSERT_TRUE(process.is_input_only_running());

  // Disabling the mode clears the cached id, but a running placeholder must stay recognized
  config::input.enable_input_only_mode = false;
  proc::input_only_app_id.clear();

  EXPECT_TRUE(process.is_input_only_running());
  EXPECT_TRUE(proc::get_input_only_app_id().empty());

  process.terminate();
  EXPECT_FALSE(process.is_input_only_running());
}

TEST_F(ProcessInputOnlyTest, ExecuteRealAppOverPlaceholderClearsInputOnlyState) {
  std::vector<proc::ctx_t> apps;
  apps.push_back(make_placeholder_ctx("780"));
  apps.push_back(make_regular_ctx("781"));

  auto process = make_process(std::move(apps));
  ASSERT_EQ(process.execute(780, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  ASSERT_TRUE(process.is_input_only_running());

  // Launching a real app terminates the placeholder, which must not leave the
  // input-only state behind for the newly launched app.
  ASSERT_EQ(process.execute(781, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  EXPECT_EQ(process.running(), 781);
  EXPECT_FALSE(process.is_input_only_running());

  process.terminate();
  EXPECT_FALSE(process.is_input_only_running());
}

TEST_F(ProcessInputOnlyTest, ResolveResumeState_SnapshotsAfterPlaceholderCleanup) {
  auto saved_global_proc = std::move(proc::proc);
  auto restore_global_proc = util::fail_guard([&saved_global_proc]() {
    proc::proc = std::move(saved_global_proc);
  });

  auto process = make_process({make_placeholder_ctx("901")});
  ASSERT_EQ(process.execute(901, std::make_shared<rtsp_stream::launch_session_t>()), 0);
  proc::proc = std::move(process);

  auto state = nvhttp::resolve_resume_state();
  EXPECT_EQ(state.app_id, 901);
  EXPECT_TRUE(state.input_only);
  EXPECT_TRUE(state.can_resume());
  EXPECT_FALSE(state.should_configure_display());

  // Model a stopping session whose teardown terminates the placeholder while the state is
  // being resolved. A snapshot taken before the cleanup would still report app 901 here
  // and let /resume fall back to desktop capture.
  state = nvhttp::resolve_resume_state([]() {
    EXPECT_EQ(proc::proc.running(), 901);
    EXPECT_TRUE(proc::proc.is_input_only_running());
    proc::proc.terminate();
    return true;
  });

  EXPECT_EQ(state.app_id, 0);
  EXPECT_FALSE(state.input_only);
  EXPECT_TRUE(state.no_active_sessions);
  EXPECT_FALSE(state.can_resume());
  EXPECT_FALSE(state.should_configure_display());
}
