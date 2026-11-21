/**
 * @file src/config_file.h
 * @brief Loading and diagnostics for an explicitly selected configuration file.
 */
#pragma once

// standard includes
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace config {
  /**
   * @brief Text or a diagnostic from loading an explicit config path.
   */
  struct explicit_config_file_t {
    std::string contents;  ///< Configuration text; may be empty even on success.
    std::string diagnostic;  ///< Nonempty when the explicit path could not supply a file.
  };

  /**
   * @brief Read an explicit config file, creating it if missing.
   * @details An empty file is valid. Never truncate an existing file.
   *
   * @param path Explicit configuration file path.
   * @return Configuration text on success, or a warning on failure.
   */
  inline explicit_config_file_t read_explicit_config_file(const std::string &path) {
    if (path.empty()) {
      return {{}, "The configuration file path argument is empty. Starting without a configuration file."};
    }

    const auto unavailable = [&] {
      return explicit_config_file_t {{}, "Could not read or create configuration file [" + path + "]. Starting without a configuration file."};
    };

    const std::filesystem::path config_path {path};
    std::error_code status_error;
    if (const auto status = std::filesystem::status(config_path, status_error); status.type() == std::filesystem::file_type::not_found) {
      std::ofstream file {config_path, std::ios::app};
      file.close();
      if (!file) {
        return unavailable();
      }
    } else if (status_error || std::filesystem::is_directory(status)) {
      return unavailable();
    }

    std::ifstream file {config_path};
    if (!file) {
      return unavailable();
    }

    explicit_config_file_t result;
    std::array<char, 4096> buffer;
    while (file.read(buffer.data(), buffer.size())) {
      result.contents.append(buffer.data(), buffer.size());
    }
    if (file.bad() || !file.eof()) {
      return unavailable();
    }
    result.contents.append(buffer.data(), file.gcount());
    return result;
  }
}  // namespace config
