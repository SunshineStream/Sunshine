/**
 * @file src/display_device.h
 * @brief Declarations for display device handling.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

// lib includes
#include <display_device/display_power_interface.h>
#include <display_device/types.h>

// forward declarations
namespace platf {
  class deinit_t;
}

namespace config {
  struct video_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace display_device {
  /**
   * @brief Initialize the implementation and perform the initial state recovery (if needed).
   * @param persistence_filepath File location for reading/saving persistent state.
   * @param video_config User's video related configuration.
   * @returns A deinit_t instance that performs cleanup when destroyed.
   *
   * @examples
   * const config::video_t &video_config { config::video };
   * const auto init_guard { init("/my/persitence/file.state", video_config) };
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init(const std::filesystem::path &persistence_filepath, const config::video_t &video_config);

  /**
   * @brief Map the output name to a specific display.
   * @param output_name The user-configurable output name.
   * @returns Mapped display name or empty string if the output name could not be mapped.
   *
   * @examples
   * const auto mapped_name_config { map_output_name(config::video.output_name) };
   * const auto mapped_name_custom { map_output_name("{some-device-id}") };
   * @examples_end
   */
  [[nodiscard]] std::string map_output_name(const std::string &output_name);

  /**
   * @brief Ask the platform to wake displays before detection or capture.
   * @param display_name Platform capture selector.
   * @param timeout Maximum time to wait for platform-specific wake detection.
   * @returns True if the display was already available or the wake request succeeded.
   */
  [[nodiscard]] bool wake_display(const std::string &display_name, std::chrono::milliseconds timeout);

  /**
   * @brief Keep displays awake until the returned guard is destroyed.
   * @param reason Short human-readable reason for the power assertion.
   * @returns A guard owning the platform assertion, or nullptr if unsupported or unavailable.
   */
  [[nodiscard]] std::unique_ptr<DisplayPowerGuardInterface> keep_display_awake(const std::string &reason);

  /**
   * @brief Configure the display device based on the user configuration and the session information.
   * @note This is a convenience method for calling similar method of a different signature.
   *
   * @param video_config User's video related configuration.
   * @param session Session information.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * configure_display(video_config, *launch_session);
   * @examples_end
   */
  void configure_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  /**
   * @brief Configure the display device using the provided configuration.
   *
   * In some cases configuring display can fail due to transient issues and
   * we will keep trying every 5 seconds, even if the stream has already started as there was
   * no possibility to apply settings before the stream start.
   *
   * Therefore, there is no return value as we still want to continue with the stream, so that
   * the users can do something about it once they are connected. Otherwise, we might
   * prevent users from logging in at all if we keep failing to apply configuration.
   *
   * @param config Configuration for the display.
   *
   * @examples
   * const SingleDisplayConfiguration valid_config { };
   * configure_display(valid_config);
   * @examples_end
   */
  void configure_display(const SingleDisplayConfiguration &config);

  /**
   * @brief Reapply the active stream's display configuration when the configured display disappears mid-stream.
   *
   * Runs synchronously when an active configuration exists, uses a preparation mode that can activate
   * displays, and is outside the reapply cooldown window.
   *
   * @return True if the active display configuration was reapplied successfully, false otherwise.
   */
  [[nodiscard]] bool reapply_configuration_on_display_loss();

  /**
   * @brief Revert the display configuration and restore the previous state.
   *
   * In case the state could not be restored, by default it will be retried again in 5 seconds
   * (repeating indefinitely until success or until persistence is reset).
   * A recovery that still has settings saved for an unavailable display also keeps retrying
   * until that display returns and those settings are restored.
   *
   * @examples
   * revert_configuration();
   * @examples_end
   */
  void revert_configuration();

  /**
   * @brief Reset persisted display state and the captured initial state.
   *
   * This is normally used to get out of the "broken" state where the algorithm wants
   * to restore the initial display state, but it is no longer possible.
   *
   * This could happen if the display is no longer available or the hardware was changed
   * and the device ids no longer match.
   *
   * The user then accepts that Sunshine is not able to restore the state and "agrees" to
   * do it manually.
   *
   * @return True if persistence was reset, false otherwise.
   * @note Whether the function succeeds or fails, any of the scheduled "retries" from
   *       other methods will be stopped to not interfere with the user actions.
   *
   * @examples
   * const auto result = reset_persistence();
   * @examples_end
   */
  [[nodiscard]] bool reset_persistence();

  /**
   * @brief Enumerate the available devices.
   * @return A list of devices.
   *
   * @examples
   * const auto devices = enumerate_devices();
   * @examples_end
   */
  [[nodiscard]] EnumeratedDeviceList enumerate_devices();

  /**
   * @brief A tag structure indicating that configuration parsing has failed.
   */
  struct failed_to_parse_tag_t {};

  /**
   * @brief A tag structure indicating that configuration is disabled.
   */
  struct configuration_disabled_tag_t {};

  /**
   * @brief Parse the user configuration and the session information.
   * @param video_config User's video related configuration.
   * @param session Session information.
   * @return Parsed single display configuration or
   *         a tag indicating that the parsing has failed or
   *         a tag indicating that the user does not want to perform any configuration.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * const auto config { parse_configuration(video_config, *launch_session) };
   * if (const auto *parsed_config { std::get_if<SingleDisplayConfiguration>(&result) }; parsed_config) {
   *    configure_display(*config);
   * }
   * @examples_end
   */
  [[nodiscard]] std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

#ifdef SUNSHINE_TESTS
  class SettingsManagerInterface;
  class SettingsPersistenceInterface;

  /**
   * @brief Create in-memory persistence that records whether settings are still stored.
   * @return Persistence object observed by the revert retry loop.
   */
  [[nodiscard]] std::shared_ptr<SettingsPersistenceInterface> test_make_watched_persistence();

  /**
   * @brief Install a settings manager and the persistence object the revert loop watches.
   * @param manager Settings manager used by later revert calls.
   * @param persistence Persistence object returned by `test_make_watched_persistence()`.
   */
  void test_install_settings_manager(std::unique_ptr<SettingsManagerInterface> manager, const std::shared_ptr<SettingsPersistenceInterface> &persistence);

  /**
   * @brief Override how long the revert loop waits between device checks.
   * @param interval Delay between attempts. Must be greater than zero.
   */
  void test_set_retry_interval(std::chrono::milliseconds interval);

  /**
   * @brief Override the minimum cooldown between mid-stream display reapply attempts.
   * @param cooldown Minimum delay between reapply attempts.
   */
  void test_set_reapply_cooldown(std::chrono::milliseconds cooldown);

  /**
   * @brief Report whether another revert attempt is still scheduled.
   * @return True when the revert scheduler will run again.
   */
  [[nodiscard]] bool test_revert_retry_is_scheduled();

  /**
   * @brief Stop the revert scheduler and drop the test settings manager.
   */
  void test_reset_display_device();
#endif
}  // namespace display_device
