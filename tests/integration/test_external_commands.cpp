/**
 * @file tests/integration/test_external_commands.cpp
 * @brief Integration tests for running external commands with platform-specific validation
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <format>
#include <string>
#include <tuple>
#include <vector>

// local includes
#include "src/compat/boost_process.h"
#include "src/platform/common.h"

// Test data structure for parameterized testing
struct ExternalCommandTestData {
  std::string command;
  std::string platform;  // "windows", "linux", "macos", or "all"
  bool should_succeed;
  std::string description;
  std::string working_directory;  // Optional: if empty, uses SUNSHINE_SOURCE_DIR
  bool xfail_condition = false;  // Optional: condition for expected failure
  std::string xfail_reason = "";  // Optional: reason for expected failure

  // Constructor with xfail parameters
  ExternalCommandTestData(std::string cmd, std::string plat, const bool succeed, std::string desc, std::string work_dir = "", const bool xfail_cond = false, std::string xfail_rsn = ""):
      command(std::move(cmd)),
      platform(std::move(plat)),
      should_succeed(succeed),
      description(std::move(desc)),
      working_directory(std::move(work_dir)),
      xfail_condition(xfail_cond),
      xfail_reason(std::move(xfail_rsn)) {}
};

class ExternalCommandTest: public BaseTest, public ::testing::WithParamInterface<ExternalCommandTestData> {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    if constexpr (IS_WINDOWS) {
      current_platform = "windows";
    } else if constexpr (IS_MACOS) {
      current_platform = "macos";
    } else if constexpr (IS_LINUX) {
      current_platform = "linux";
    }
  }

  [[nodiscard]] bool shouldRunOnCurrentPlatform(const std::string_view &test_platform) const {
    return test_platform == "all" || test_platform == current_platform;
  }

  // Helper function to run a command using the existing process infrastructure
  static std::pair<int, std::string> runCommand(const std::string &cmd, const std::string_view &working_dir) {
    const auto env = boost::this_process::environment();

    // Determine the working directory: use the provided working_dir or fall back to SUNSHINE_SOURCE_DIR
    boost::filesystem::path effective_working_dir;

    if (!working_dir.empty()) {
      effective_working_dir = working_dir;
    } else {
      // Use SUNSHINE_SOURCE_DIR CMake definition as the default working directory
      effective_working_dir = SUNSHINE_SOURCE_DIR;
    }

    std::error_code ec;

    // Create a temporary file to capture output
    const auto temp_file = std::tmpfile();
    if (!temp_file) {
      return {-1, "Failed to create temporary file for output"};
    }

    // Run the command using the existing platf::run_command function
    auto child = platf::run_command(
      false,  // not elevated
      false,  // not interactive
      cmd,
      effective_working_dir,
      env,
      temp_file,
      ec,
      nullptr  // no process group
    );

    if (ec) {
      std::fclose(temp_file);
      return {-1, std::format("Failed to start command: {}", ec.message())};
    }

    // Wait for the command to complete
    child.wait();
    int exit_code = child.exit_code();

    // Read the output from the temporary file
    std::rewind(temp_file);
    std::string output;
    std::array<char, 1024> buffer {};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), temp_file)) {
      // std::string constructor automatically handles null-terminated strings
      output += std::string(buffer.data());
    }
    std::fclose(temp_file);

    return {exit_code, output};
  }

public:
  std::string current_platform;
};

// Test case implementation
TEST_P(ExternalCommandTest, RunExternalCommand) {
  const auto &[command, platform, should_succeed, description, working_directory, xfail_condition, xfail_reason] = GetParam();

  // Skip test if not for the current platform
  if (!shouldRunOnCurrentPlatform(platform)) {
    GTEST_SKIP() << "Test not applicable for platform: " << current_platform;
  }

  // Use the xfail condition and reason from test data
  XFAIL_IF(xfail_condition, xfail_reason);

  BOOST_LOG(info) << "Running external command test: " << description;
  BOOST_LOG(debug) << "Command: " << command;

  auto [exit_code, output] = runCommand(command, working_directory);

  BOOST_LOG(debug) << "Command exit code: " << exit_code;
  if (!output.empty()) {
    BOOST_LOG(debug) << "Command output: " << output;
  }

  if (should_succeed) {
    HANDLE_XFAIL_ASSERT_EQ(exit_code, 0, std::format("Command should have succeeded but failed with exit code {}\nOutput: {}", std::to_string(exit_code), output));
  } else {
    HANDLE_XFAIL_ASSERT_NE(exit_code, 0, std::format("Command should have failed but succeeded\nOutput: {}", output));
  }
}

// Platform-specific command strings
constexpr auto SIMPLE_COMMAND = IS_WINDOWS ? "where cmd" : "which sh";

#ifdef UDEVADM_EXECUTABLE
  #define UDEV_TESTS \
    ExternalCommandTestData { \
      std::format("{} verify {}/src_assets/linux/misc/60-sunshine.rules", UDEVADM_EXECUTABLE, SUNSHINE_TEST_BIN_DIR), \
      "linux", \
      true, \
      "Test udev rules file" \
    },
#else
  #define UDEV_TESTS
#endif

// Test data
INSTANTIATE_TEST_SUITE_P(
  ExternalCommands,
  ExternalCommandTest,
  ::testing::Values(
    UDEV_TESTS
      ExternalCommandTestData {
        "sh tests/scripts/test_additional_install.sh success",
        "linux",
        true,
        "Flatpak installer preserves packaged files"
      },
    ExternalCommandTestData {
      "sh tests/scripts/test_additional_install.sh literal",
      "linux",
      true,
      "Flatpak installer preserves literal shell syntax"
    },
    ExternalCommandTestData {
      "sh tests/scripts/test_additional_install.sh modules",
      "linux",
      true,
      "Flatpak installer stops after module configuration failure"
    },
    ExternalCommandTestData {
      "sh tests/scripts/test_additional_install.sh missing",
      "linux",
      true,
      "Flatpak installer stops after missing udev rules"
    },
    ExternalCommandTestData {
      "sh tests/scripts/test_additional_install.sh rules",
      "linux",
      true,
      "Flatpak installer stops after udev installation failure"
    },
    ExternalCommandTestData {
      "sh tests/scripts/test_additional_install.sh reload",
      "linux",
      true,
      "Flatpak installer stops after udev reload failure"
    },
    // Cross-platform tests with xfail on Windows CI
    ExternalCommandTestData {
      SIMPLE_COMMAND,
      "all",
      true,
      "Simple command test",
      "",  // working_directory
      IS_WINDOWS,  // xfail_condition
      "Simple command test fails on Windows CI environment"  // xfail_reason
    },
    // Cross-platform failing test
    ExternalCommandTestData {
      "non_existent_command_12345",
      "all",
      false,
      "Test command that should fail"
    }
  ),
  [](const ::testing::TestParamInfo<ExternalCommandTestData> &info) {
    // Generate test names from a description
    std::string name = info.param.description;
    // Replace spaces and special characters with underscores for valid test names
    std::replace_if(name.begin(), name.end(), [](char c) {
      return !std::isalnum(c);
    },
                    '_');
    return name;
  }
);
