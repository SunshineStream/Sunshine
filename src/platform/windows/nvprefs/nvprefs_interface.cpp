/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.cpp
 * @brief Definitions for nvidia preferences interface.
 */
// standard includes
#include <array>
#include <cassert>

// local includes
#include "driver_settings.h"
#include "nvprefs_interface.h"
#include "undo_file.h"

namespace {

  const auto sunshine_program_data_folder = "Sunshine";
  const auto nvprefs_undo_file_name = "nvprefs_undo.json";

}  // namespace

namespace nvprefs {

  /**
   * @brief Private state owned by the NVIDIA preferences interface.
   */
  struct nvprefs_interface::impl {
    bool loaded = false;  ///< Whether NVIDIA preference state has been loaded.
    driver_settings_t driver_settings;  ///< Driver settings.
    std::filesystem::path undo_folder_path;  ///< Undo folder path.
    std::filesystem::path undo_file_path;  ///< Undo file path.
    std::optional<undo_data_t> undo_data;  ///< Undo data.
    std::optional<undo_file_t> undo_file;  ///< Undo file.
  };

  nvprefs_interface::nvprefs_interface():
      pimpl(new impl()) {
  }

  nvprefs_interface::~nvprefs_interface() {
    if (owning_undo_file() && load()) {
      restore_global_profile();
    }
    unload();
  }

  bool nvprefs_interface::load() {
    if (!pimpl->loaded) {
      // Check %ProgramData% variable, need it for storing undo file
      std::array<wchar_t, MAX_PATH> program_data_env;
      if (auto get_env_result = GetEnvironmentVariableW(L"ProgramData", program_data_env.data(), program_data_env.size()); get_env_result == 0 || get_env_result >= MAX_PATH || !std::filesystem::is_directory(program_data_env.data())) {
        error_message("Missing or malformed %ProgramData% environment variable");
        return false;
      }

      // Prepare undo file path variables
      pimpl->undo_folder_path = std::filesystem::path(program_data_env.data()) / sunshine_program_data_folder;
      pimpl->undo_file_path = pimpl->undo_folder_path / nvprefs_undo_file_name;

      // Dynamically load nvapi library and load driver settings
      pimpl->loaded = pimpl->driver_settings.init();
    }

    return pimpl->loaded;
  }

  void nvprefs_interface::unload() {
    if (pimpl->loaded) {
      // Unload dynamically loaded nvapi library
      pimpl->driver_settings.destroy();
      pimpl->loaded = false;
    }
  }

  bool nvprefs_interface::restore_from_and_delete_undo_file_if_exists() {
    if (!pimpl->loaded) {
      return false;
    }

    // Check for undo file from previous improper termination
    bool access_denied = false;
    if (auto undo_file = undo_file_t::open_existing_file(pimpl->undo_file_path, access_denied)) {
      // Try to restore from the undo file
      info_message("Opened undo file from previous improper termination");
      if (auto undo_data = undo_file->read_undo_data()) {
        if (pimpl->driver_settings.restore_global_profile_to_undo(*undo_data) && pimpl->driver_settings.save_settings()) {
          info_message("Restored global profile settings from undo file - deleting the file");
        } else {
          error_message("Failed to restore global profile settings from undo file, deleting the file anyway");
        }
      } else {
        error_message("Coulnd't read undo file, deleting the file anyway");
      }

      if (!undo_file->delete_file()) {
        error_message("Couldn't delete undo file");
        return false;
      }
    } else if (access_denied) {
      error_message("Couldn't open undo file from previous improper termination, or confirm that there's no such file");
      return false;
    }

    return true;
  }

  bool nvprefs_interface::modify_application_profile() {
    if (!pimpl->loaded) {
      return false;
    }

    // Modify and save sunshine.exe application profile settings, if needed
    if (bool modified = false; !pimpl->driver_settings.check_and_modify_application_profile(modified)) {
      error_message("Failed to modify application profile settings");
      return false;
    } else if (modified) {
      if (pimpl->driver_settings.save_settings()) {
        info_message("Modified application profile settings");
      } else {
        error_message("Couldn't save application profile settings");
        return false;
      }
    } else {
      info_message("No need to modify application profile settings");
    }

    return true;
  }

  bool nvprefs_interface::modify_global_profile() {
    if (!pimpl->loaded) {
      return false;
    }

    // Modify but not save global profile settings, if needed
    std::optional<undo_data_t> undo_data;
    if (!pimpl->driver_settings.check_and_modify_global_profile(undo_data)) {
      error_message("Couldn't modify global profile settings");
      return false;
    } else if (!undo_data) {
      info_message("No need to modify global profile settings");
      return true;
    }

    auto make_undo_and_commit = [&]() {
      // Create and lock undo file if it hasn't been done yet
      if (!pimpl->undo_file) {
        // Prepare Sunshine folder in ProgramData if it doesn't exist
        if (!CreateDirectoryW(pimpl->undo_folder_path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
          error_message("Couldn't create undo folder");
          return false;
        }

        // Create undo file to handle improper termination of nvprefs.exe
        pimpl->undo_file = undo_file_t::create_new_file(pimpl->undo_file_path);
        if (!pimpl->undo_file) {
          error_message("Couldn't create undo file");
          return false;
        }
      }

      assert(undo_data);
      if (pimpl->undo_data) {
        // Merge undo data if settings has been modified externally since our last modification
        pimpl->undo_data->merge(*undo_data);
      } else {
        pimpl->undo_data = undo_data;
      }

      // Write undo data to undo file
      if (!pimpl->undo_file->write_undo_data(*pimpl->undo_data)) {
        error_message("Couldn't write to undo file - deleting the file");
        if (!pimpl->undo_file->delete_file()) {
          error_message("Couldn't delete undo file");
        }
        return false;
      }

      // Save global profile settings
      if (!pimpl->driver_settings.save_settings()) {
        error_message("Couldn't save global profile settings");
        return false;
      }

      return true;
    };

    if (!make_undo_and_commit()) {
      // Revert settings modifications
      pimpl->driver_settings.load_settings();
      return false;
    }

    return true;
  }

  bool nvprefs_interface::owning_undo_file() const {
    return pimpl->undo_file.has_value();
  }

  bool nvprefs_interface::restore_global_profile() {
    if (!pimpl->loaded || !pimpl->undo_data || !pimpl->undo_file) {
      return false;
    }

    // Restore global profile settings with undo data
    if (pimpl->driver_settings.restore_global_profile_to_undo(*pimpl->undo_data) && pimpl->driver_settings.save_settings()) {
      // Global profile settings successfully restored, can delete undo file
      if (!pimpl->undo_file->delete_file()) {
        error_message("Couldn't delete undo file");
        return false;
      }
      pimpl->undo_data = std::nullopt;
      pimpl->undo_file = std::nullopt;
    } else {
      error_message("Couldn't restore global profile settings");
      return false;
    }

    return true;
  }

}  // namespace nvprefs
