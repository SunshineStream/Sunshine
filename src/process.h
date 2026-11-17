/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  /**
   * @def __kernel_entry
   * @brief Macro for kernel entry.
   */
  #define __kernel_entry
#endif

// standard includes
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "src/compat/boost_process.h"
#include "utility.h"

/**
 * @def DEFAULT_APP_IMAGE_PATH
 * @brief Macro for DEFAULT APP IMAGE PATH.
 */
#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  /**
   * @brief Boost.Process pipe stream used for child-process I/O.
   */
  using file_t = util::safe_ptr_v2<FILE, int, &fclose>;

  /**
   * @brief Parsed command arguments used when launching a child process.
   */
  using cmd_t = config::prep_cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;  ///< Prep cmds.

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string name;  ///< Human-readable name for this item.
    std::string cmd;  ///< Command line used to launch the application.
    std::string working_dir;  ///< Working dir.
    std::string output;  ///< Captured output from the launched process.
    std::string image_path;  ///< Image path.
    std::string id;  ///< Stable identifier for the configured application.
    bool elevated;  ///< Whether the process should be launched elevated.
    bool auto_detach;  ///< Whether the process should detach automatically.
    bool wait_all;  ///< Whether Sunshine waits for all child processes.
    std::chrono::seconds exit_timeout;  ///< Exit timeout.
  };

  /**
   * @brief Tracks launched child processes and terminates them during shutdown.
   */
  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    /**
     * @brief Construct a process manager.
     *
     * @param env Environment used when launching processes.
     * @param apps Application launch contexts.
     */
    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    /**
     * @brief Launch the configured application process.
     *
     * @param app_id App ID.
     * @param launch_session Launch session.
     * @return Process exit code or launch error status.
     */
    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    /**
     * @brief Return the operating-system PID of the launched application root process.
     *
     * Returns 0 for the Desktop/placebo session, when no application has been
     * launched, and when the root process is no longer running. Callers use 0
     * to fall back to ordinary endpoint audio capture. Implemented via
     * app_process_identity() so the PID always agrees with the handed-off identity.
     *
     * @return Launched root process PID, or 0 when unavailable.
     */
    std::uint32_t app_process_id();

    /**
     * @brief Return the operating-system identity of the launched application root process.
     *
     * The PID and creation marker are captured together from the owned child
     * handle under the lifecycle lock, so the pair always names the launched
     * process even if its PID is later reused. Returns {0, 0} for the
     * Desktop/placebo session, when no application has been launched, when the
     * root process is no longer running, and when the creation marker cannot
     * be queried. Callers use a zero PID to fall back to ordinary endpoint
     * audio capture.
     *
     * @return Launched root process identity, or {0, 0} when unavailable.
     */
    platf::process_identity_t app_process_identity();

    ~proc_t();

    /**
     * @brief Return a copy of the configured applications.
     *
     * The copy is taken under the lifecycle mutex, so callers may iterate it
     * without racing `update_apps_and_env()`.
     *
     * @return Copy of the application list owned by the process manager.
     */
    std::vector<ctx_t> get_apps() const;
    /**
     * @brief Return the configured applications.
     *
     * The mutable reference is for configuration reload and test setup only;
     * callers must not hold it across lifecycle operations on other threads.
     *
     * @return Mutable application list owned by the process manager.
     */
    std::vector<ctx_t> &get_apps();
    /**
     * @brief Get app image.
     *
     * @param app_id App ID.
     * @return Validated image path for the requested application.
     */
    std::string get_app_image(int app_id);
    /**
     * @brief Get last run app name.
     *
     * @return Name of the most recently launched application.
     */
    std::string get_last_run_app_name();
    /**
     * @brief Terminate the launched application process.
     */
    void terminate();

    /**
     * @brief Update the cached app list and environment without disturbing running process state.
     *
     * This replaces only the application configuration data (_apps and _env) from a newly parsed
     * proc_t, preserving the current _app_id, _process, _process_group, and other fields that
     * track an in-flight streaming session.
     *
     * @param other The newly parsed proc_t whose apps and env will be moved into this instance.
     */
    void update_apps_and_env(proc_t &&other);

#ifdef SUNSHINE_TESTS
    /**
     * @brief Get the current process environment (strictly for unit tests).
     */
    const boost::process::v1::environment &get_env() const {
      return _env;
    }
#endif

  private:
    /**
     * @brief Implementation of execute() that calls terminate_impl() directly for internal cleanup.
     *
     * @param app_id App ID.
     * @param launch_session Launch session.
     * @return Process exit code or launch error status.
     */
    int execute_impl(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @brief Implementation of running() that may call terminate_impl() directly for cleanup.
     *
     * @return `_app_id` if a process is running, otherwise returns `0`.
     */
    int running_impl();

    /**
     * @brief State claimed atomically for one termination.
     *
     * `claim_termination_locked()` moves the process handles and copies the
     * app/environment out under the lifecycle mutex; `finish_termination()`
     * then performs every wait and undo command on this snapshot without
     * holding the mutex, so a successor app launched in the meantime is
     * never terminated or undone by mistake.
     */
    struct termination_snapshot_t {
      boost::process::v1::child process;  ///< Launched root process claimed for termination.
      boost::process::v1::group process_group;  ///< Process group claimed for termination.
      ctx_t app;  ///< Copy of the app context whose prep commands are undone.
      boost::process::v1::environment env;  ///< Copy of the environment for undo commands.
      file_t pipe;  ///< Output pipe claimed with the app, closed after the undo commands.
      std::size_t prep_completed {};  ///< Number of leading prep commands to undo, in reverse order.
      std::chrono::seconds exit_timeout {};  ///< Graceful-exit timeout copied from the app context.
      bool has_run {};  ///< Whether an app had been launched when the claim was taken.
      std::string app_name;  ///< Name of the claimed app, for the stopped notification.
    };

    /**
     * @brief Claim the current process state for termination.
     *
     * The caller must hold the lifecycle mutex. Claiming bumps
     * `_lifecycle_generation`, moves `_process`/`_process_group`/`_pipe`
     * out, and copies `_app`/`_env`, so the decision to clean up and the
     * state being cleaned up are one atomic step.
     *
     * @return Snapshot carrying everything `finish_termination()` needs.
     */
    termination_snapshot_t claim_termination_locked();

    /**
     * @brief Terminate a claimed snapshot and run its undo commands.
     *
     * Runs without the lifecycle mutex: every wait operates on the snapshot,
     * never on member state a successor launch may already own.
     *
     * @param snapshot State previously returned by `claim_termination_locked()`.
     */
    void finish_termination(termination_snapshot_t &&snapshot);

    /**
     * @brief Implementation of terminate() used by public wrappers and internal callers.
     *
     * @param expected_generation When set, terminate only when the lifecycle
     * generation still matches, so stale cleanup cannot kill a successor app.
     */
    void terminate_impl(std::optional<std::uint64_t> expected_generation = std::nullopt);

    int _app_id;
    std::uint64_t _lifecycle_generation {};  ///< Bumped on every launch claim and termination claim, guarded by the lifecycle mutex.
    std::size_t _app_prep_completed {};  ///< Prep commands completed for `_app`, guarded by the lifecycle mutex.

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
  };

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   *
   * @param app_name App name.
   * @param app_image_path App image path.
   * @param index Zero-based index of the item being addressed.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  /**
   * @brief Prepare a configured command for execution.
   *
   * @param command Configured command line.
   * @return Command line with any package-specific launcher prefix applied.
   */
  std::string prepare_command(const std::string &command);

  bool check_valid_png(const std::filesystem::path &path);
  /**
   * @brief Validate app image path.
   *
   * @param app_image_path Candidate image path from the application configuration.
   * @return Existing PNG path, or the default application image when validation fails.
   */
  std::string validate_app_image_path(std::string app_image_path);
  /**
   * @brief Refresh cached platform state from the operating system.
   *
   * @param file_name File name.
   */
  void refresh(const std::string &file_name);
  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param file_name File name.
   * @return Parsed value or parse status.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param child_process The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &child_process, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;

  /**
   * @brief Build the capture process-identity provider handed to audio capture.
   *
   * The returned callable reports `target.app_process_identity()` at each call, so
   * audio (re)initialization always sees the currently launched app. It
   * borrows `target`; the caller keeps it alive for the provider's lifetime.
   *
   * @param target Process manager whose launched app identity is reported.
   * @return Provider returning the launched root identity, or {0, 0} when unavailable.
   */
  std::function<platf::process_identity_t()> app_process_identity_provider(proc_t &target);
}  // namespace proc
