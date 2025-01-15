/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

#include <optional>
#include <unordered_map>

#include <boost/process/v1.hpp>

#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

  typedef config::prep_cmd_t cmd_t;
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
    std::vector<cmd_t> prep_cmds;

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

    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    bool elevated;
    bool auto_detach;
    bool wait_all;
    std::chrono::seconds exit_timeout;
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {}

    int
    execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int
    running();

    ~proc_t();

    const std::vector<ctx_t> &
    get_apps() const;
    std::vector<ctx_t> &
    get_apps();
    void
    set_apps(std::vector<ctx_t> apps);
    std::string
    get_app_image(int app_id);
    std::string
    get_last_run_app_name();
    const boost::process::v1::environment &
    get_env() const;
    boost::process::v1::environment &
    get_env();
    void
    set_env(boost::process::v1::environment env);
    void
    terminate();

  private:
    int _app_id;

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
   */
  std::tuple<std::string, std::string>
  calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  std::string
  validate_app_image_path(std::string app_image_path);
  void
  refresh(const std::string &file_name);
  std::optional<proc::proc_t>
  parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t>
  init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void
  terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
