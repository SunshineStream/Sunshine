/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/token_functions.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "input.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "system_tray.h"
#include "utility.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/utf_utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;  ///< Global process registry used to track and terminate child processes.

  /**
   * @brief Serialize access to proc_t lifecycle state shared across threads.
   *
   * Lock order: proc_transition_mutex() is always taken before this mutex,
   * and audio::capture_target_mutex() is always taken before this mutex
   * (the capture process-identity provider must be invoked under that mutex),
   * and this mutex is never taken in the reverse order.
   * This mutex is never held across blocking process waits, prep/undo
   * command waits, or display/input calls: lifecycle operations snapshot
   * or claim the shared state under this mutex and work on locals after
   * releasing it. The only process queries made under it are the
   * non-blocking `child::running()`/`native_exit_code()` checks and the
   * non-blocking `platf::process_group_running()` group query.
   *
   * @return Reference to the process-lifetime lifecycle mutex.
   */
  std::mutex &proc_lifecycle_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t: public platf::deinit_t {
  public:
    /**
     * @brief Destroy the process subsystem deinitializer.
     */
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &child_process, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (child_process.valid()) {
      // avoid zombie process
      child_process.detach();
    }
  }

  /**
   * @brief Resolve the working directory for a configured command.
   *
   * @param cmd Command line to execute or inspect.
   * @param env Environment variables for the child process.
   * @return Directory used to launch the command, falling back to PATH lookup when needed.
   */
  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
    std::vector<std::string> parts;
    try {
#ifdef _WIN32
      parts = boost::program_options::split_winmain(cmd);
#else
      parts = boost::program_options::split_unix(cmd);
#endif
    } catch (boost::escaped_list_error &err) {
      BOOST_LOG(error) << "Boost failed to parse command ["sv << cmd << "] because " << err.what();
      return boost::filesystem::path();
    }
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).contains("://")) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      // Search the child's PATH. Boost's default search list reads this process's environment through a
      // std::locale("") on POSIX, which throws when an LC_* variable names a locale that is not installed.
      if (const auto path_var = env.find("PATH"); path_var != env.end()) {
        const auto path_entries = path_var->to_vector();
        const std::vector<boost::filesystem::path> search_dirs {path_entries.begin(), path_entries.end()};
        cmd_path = boost::process::v1::search_path(parts.at(0), search_dirs);
      } else {
        cmd_path = boost::process::v1::search_path(parts.at(0));
      }
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  std::string prepare_command(const std::string &command) {
#ifdef SUNSHINE_BUILD_FLATPAK
    constexpr auto host_command_prefix = "flatpak-spawn --host "sv;
    const auto stripped_command = boost::trim_copy(command);
    if (stripped_command.empty() || stripped_command.starts_with(host_command_prefix)) {
      if (!stripped_command.empty()) {
        BOOST_LOG(warning) << "Command ["sv << stripped_command << "] already includes [flatpak-spawn --host], which is no longer required for Flatpak builds."sv;
      }
      return stripped_command;
    }

    return std::string {host_command_prefix} + stripped_command;
#else
    return command;
#endif
  }

  /**
   * @brief Check a prep-command launch and wait for successful completion.
   *
   * @param child Process returned by the prep-command launch, or an empty child on failure.
   * @param command Command line used for logging.
   * @param ec Launch error, updated by the wait or cleared when permission denial is allowed.
   * @param allow_permission_denied Whether a permission-denied launch may be skipped for Desktop access before login.
   * @return True if the command succeeded or its permission-denied launch was allowed.
   */
  bool wait_for_prep_command(boost::process::v1::child &child, const std::string &command, std::error_code &ec, bool allow_permission_denied) {
    if (ec) {
      BOOST_LOG(error) << "Couldn't run ["sv << command << "]: System: "sv << ec.message();
      // User impersonation can fail before anyone signs in after a reboot. Allow Desktop access
      // without waiting on the child that never started or passing its launch error to later commands.
      if (allow_permission_denied && ec == std::errc::permission_denied) {
        ec.clear();
        return true;
      }
      return false;
    }

    child.wait(ec);
    if (ec) {
      BOOST_LOG(error) << '[' << command << "] wait failed with error code ["sv << ec << ']';
      return false;
    }
    auto ret = child.exit_code();
    if (ret != 0) {
      BOOST_LOG(error) << '[' << command << "] exited with code ["sv << ret << ']';
      return false;
    }
    return true;
  }

  /**
   * @brief Serialize complete launch and cleanup transitions independently of state reads.
   *
   * @return Process-lifetime transition mutex.
   */
  std::mutex &proc_transition_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    std::lock_guard transition {proc_transition_mutex()};
    return execute_impl(app_id, std::move(launch_session));
  }

  int proc_t::execute_impl(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    // Ensure starting from a clean slate
    terminate_impl();

    // Snapshot the app and environment under the lock, then run every
    // prep/detached/launch wait on the locals. Shared state is only
    // republished in short locked sections that verify `generation`, so a
    // concurrent terminate or successor launch is never overwritten.
    ctx_t app;
    boost::process::v1::environment env;
    std::uint64_t generation = 0;
    {
      std::lock_guard lock {proc_lifecycle_mutex()};
      auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto &listed_app) {
        return listed_app.id == std::to_string(app_id);
      });

      if (iter == _apps.end()) {
        BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
        return 404;
      }

      _app_id = app_id;
      _app = *iter;
      _app_prep_begin = std::begin(_app.prep_cmds);
      _app_prep_it = _app_prep_begin;
      _app_prep_completed = 0;
      generation = ++_lifecycle_generation;
      app = _app;
      env = _env;
    }

    // Add Stream-specific environment variables
    env["SUNSHINE_APP_ID"] = std::to_string(app_id);
    env["SUNSHINE_APP_NAME"] = app.name;
    env["SUNSHINE_CLIENT_NAME"] = launch_session->client_name;
    env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    {
      // Publish the session environment so a concurrent terminate undoes
      // prep commands with it and `update_apps_and_env()` preserves it.
      std::lock_guard lock {proc_lifecycle_mutex()};
      if (_lifecycle_generation != generation) {
        return -1;
      }
      _env = env;
    }

    // The pipe stays local until the launch is published, so a concurrent
    // terminate can never close a FILE this launch is still spawning with.
    file_t pipe;
    if (!app.output.empty() && app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = utf_utils::from_utf8(app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      pipe.reset(fopen(app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    // Executed when returning from function; generation-checked so failing
    // cleanup for this launch cannot terminate a successor app.
    auto fg = util::fail_guard([this, generation]() {
      terminate_impl(generation);
    });

    for (std::size_t prep_index = 0; prep_index < app.prep_cmds.size(); ++prep_index) {
      const auto &cmd = app.prep_cmds[prep_index];

      // Skip empty commands
      if (!cmd.do_cmd.empty()) {
        const auto command = prepare_command(cmd.do_cmd);
        boost::filesystem::path working_dir = app.working_dir.empty() ?
                                                find_working_directory(command, env) :
                                                boost::filesystem::path(app.working_dir);
        BOOST_LOG(info) << "Executing Do Cmd: ["sv << command << ']';
        auto child = platf::run_command(cmd.elevated, true, command, working_dir, env, pipe.get(), ec, nullptr);

        if (!wait_for_prep_command(child, command, ec, app.cmd.empty())) {
          return -1;
        }
      }

      std::lock_guard lock {proc_lifecycle_mutex()};
      if (_lifecycle_generation != generation) {
        return -1;
      }
      _app_prep_completed = prep_index + 1;
      _app_prep_it = std::begin(_app.prep_cmds) + static_cast<std::ptrdiff_t>(_app_prep_completed);
    }

    for (auto &cmd : app.detached) {
      const auto command = prepare_command(cmd);
      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(command, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << command << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(app.elevated, true, command, working_dir, env, pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << command << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      std::lock_guard lock {proc_lifecycle_mutex()};
      if (_lifecycle_generation != generation) {
        return -1;
      }
      placebo = true;
      _pipe = std::move(pipe);
      _app_launch_time = std::chrono::steady_clock::now();
    } else {
      const auto command = prepare_command(app.cmd);
      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(command, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << command << "] in ["sv << working_dir << ']';
      boost::process::v1::group launched_group;
      auto launched_process = platf::run_command(app.elevated, true, command, working_dir, env, pipe.get(), ec, &launched_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << command << "]: System: "sv << ec.message();
        return -1;
      }
      bool superseded = false;
      {
        std::lock_guard lock {proc_lifecycle_mutex()};
        if (_lifecycle_generation != generation) {
          superseded = true;
        } else {
          _process = std::move(launched_process);
          _process_group = std::move(launched_group);
          _pipe = std::move(pipe);
          _app_launch_time = std::chrono::steady_clock::now();
        }
      }
      if (superseded) {
        // A terminate or successor launch claimed the lifecycle while this
        // process was starting; stop only the process launched here.
        terminate_process_group(launched_process, launched_group, app.exit_timeout);
        return -1;
      }
    }

    fg.disable();

    return 0;
  }

  std::uint32_t proc_t::app_process_id() {
    return app_process_identity().process_id;
  }

  /**
   * @brief Return the operating-system identity of the launched application root process.
   */
  platf::process_identity_t proc_t::app_process_identity() {
    std::lock_guard lock {proc_lifecycle_mutex()};
    if (placebo || !_process.valid() || !_process.running()) {
      return {};
    }
#ifdef _WIN32
    // Query the creation marker from the owned child handle while the lock pins
    // it to the launched process: even if the PID is reused later, the recorded
    // baseline still names this process, so revalidation rejects the replacement.
    FILETIME creation_time {};
    FILETIME exit_time {};
    FILETIME kernel_time {};
    FILETIME user_time {};
    if (!GetProcessTimes(_process.native_handle(), &creation_time, &exit_time, &kernel_time, &user_time)) {
      return {};
    }
    const auto combined_creation_time = (static_cast<std::uint64_t>(creation_time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(creation_time.dwLowDateTime);
    if (combined_creation_time == 0) {
      return {};
    }
    return {static_cast<std::uint32_t>(_process.id()), combined_creation_time};
#else
    // POSIX backends never select per-process capture, so no creation marker is needed.
    return {static_cast<std::uint32_t>(_process.id()), 0};
#endif
  }

  int proc_t::running() {
    std::lock_guard transition {proc_transition_mutex()};
    return running_impl();
  }

  int proc_t::running_impl() {
    // Snapshot the decision inputs under the lock, then re-verify the
    // generation before committing. The cleanup claim happens in the same
    // lock hold as the final decision, so a successor launched in between
    // is never the process cleaned up. The `wait_all` group query runs
    // under the lock as well: on POSIX it reaps via `waitpid()`, so leaving
    // it unlocked would let a concurrent poll's reaper steal the root
    // child's exit status before `_process.running()` captures it.
    struct running_snapshot_t {
      int app_id {};  ///< App id observed in the snapshot.
      std::uint64_t generation {};  ///< Lifecycle generation observed in the snapshot.
      bool placebo {};  ///< Whether the Desktop placebo session was active.
    };

    // On POSIX, reap exited children on every poll (including the snapshot
    // early returns below), as HEAD did; serialize against `bp::wait()` and
    // `platf::process_group_running()` via the lifecycle mutex.
#ifndef _WIN32
    auto reaper = util::fail_guard([]() {
      std::lock_guard lock {proc_lifecycle_mutex()};
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    running_snapshot_t snapshot;
    {
      std::lock_guard lock {proc_lifecycle_mutex()};
      snapshot.app_id = _app_id;
      snapshot.generation = _lifecycle_generation;
      snapshot.placebo = placebo;
    }

    if (snapshot.placebo) {
      return snapshot.app_id;
    }

    std::optional<termination_snapshot_t> claimed;
    {
      std::lock_guard lock {proc_lifecycle_mutex()};
      if (_lifecycle_generation != snapshot.generation) {
        // State changed while unlocked; report the current app, if any.
        return _app_id > 0 ? _app_id : 0;
      }
      if (placebo) {
        return _app_id;
      } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
        // The app is still running if any process in the group is still running.
        return _app_id;
      } else if (_process.running()) {
        // The app is still running only if the initial process launched is still running
        return _app_id;
      } else if (_app.auto_detach && _process.native_exit_code() == 0 && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
        BOOST_LOG(info) << "App exited gracefully within 5 seconds of launch. Treating the app as a detached command."sv;
        BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
        placebo = true;
        return _app_id;
      }

      // Claim cleanup atomically with the decision: the move-out below is
      // what a later terminate would have raced with a successor launch.
      if (_process) {
        BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << ']';
        claimed = claim_termination_locked();
      }
    }

    if (claimed) {
      finish_termination(std::move(*claimed));
    }

    return 0;
  }

  void proc_t::terminate() {
    std::lock_guard transition {proc_transition_mutex()};
    terminate_impl();
  }

  auto proc_t::claim_termination_locked() -> termination_snapshot_t {
    termination_snapshot_t snapshot;
    ++_lifecycle_generation;
    placebo = false;
    snapshot.process = std::move(_process);
    _process = boost::process::v1::child();
    snapshot.process_group = std::move(_process_group);
    _process_group = boost::process::v1::group();
    snapshot.app = _app;
    snapshot.env = _env;
    snapshot.pipe = std::move(_pipe);
    _pipe = file_t {};
    snapshot.prep_completed = std::min(_app_prep_completed, snapshot.app.prep_cmds.size());
    _app_prep_completed = 0;
    _app_prep_it = _app_prep_begin;
    snapshot.exit_timeout = _app.exit_timeout;
    snapshot.has_run = _app_id > 0;
    snapshot.app_name = _app.name;
    _app_id = -1;
    return snapshot;
  }

  void proc_t::finish_termination(termination_snapshot_t &&snapshot) {
    input::terminate_gamepads();
    std::error_code ec;
    terminate_process_group(snapshot.process, snapshot.process_group, snapshot.exit_timeout);

    for (std::size_t prep_index = snapshot.prep_completed; prep_index > 0; --prep_index) {
      const auto &cmd = snapshot.app.prep_cmds[prep_index - 1];

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      const auto command = prepare_command(cmd.undo_cmd);
      boost::filesystem::path working_dir = snapshot.app.working_dir.empty() ?
                                              find_working_directory(command, snapshot.env) :
                                              boost::filesystem::path(snapshot.app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << command << ']';
      auto child = platf::run_command(cmd.elevated, true, command, working_dir, snapshot.env, snapshot.pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    snapshot.pipe.reset();

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (!snapshot.app_name.empty() && snapshot.has_run) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(snapshot.app_name);
#endif

      display_device::revert_configuration();
    }
  }

  void proc_t::terminate_impl(std::optional<std::uint64_t> expected_generation) {
    std::optional<termination_snapshot_t> claimed;
    {
      std::lock_guard lock {proc_lifecycle_mutex()};
      if (expected_generation && *expected_generation != _lifecycle_generation) {
        return;
      }
      claimed = claim_termination_locked();
    }
    finish_termination(std::move(*claimed));
  }

  std::function<platf::process_identity_t()> app_process_identity_provider(proc_t &target) {
    return [&target]() {
      return target.app_process_identity();
    };
  }

  std::vector<ctx_t> proc_t::get_apps() const {
    std::lock_guard lock {proc_lifecycle_mutex()};
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    std::string app_image_path;
    {
      std::lock_guard lock {proc_lifecycle_mutex()};
      auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto &listed_app) {
        return listed_app.id == std::to_string(app_id);
      });
      app_image_path = iter == _apps.end() ? std::string() : iter->image_path;
    }

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    std::lock_guard lock {proc_lifecycle_mutex()};
    return _app.name;
  }

  void proc_t::update_apps_and_env(proc_t &&other) {
    std::lock_guard lock {proc_lifecycle_mutex()};
    if (_app_id > 0 || placebo) {
      // Preserve session-specific environment variables for the running app
      for (const auto &var : _env) {
        std::string name = var.get_name();
        if (name.find("SUNSHINE_APP_") == 0 || name.find("SUNSHINE_CLIENT_") == 0) {
          other._env[name] = var.to_string();
        }
      }
    }
    _env = std::move(other._env);
    _apps = std::move(other._apps);
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  /**
   * @brief Find the closing parenthesis for an environment-variable expression.
   *
   * @param begin Iterator positioned at the opening parenthesis.
   * @param end End iterator for the expression being scanned.
   * @return Iterator for the matching closing parenthesis, or end when unmatched.
   */
  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  /**
   * @brief Parse env val.
   *
   * @param env Environment variables for the child process.
   * @param val_raw Raw value that may contain $(NAME) substitutions.
   * @return Value with recognized environment-variable substitutions expanded.
   */
  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = boost::range::find_if(std::as_const(env), [&var_name](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  /**
   * @brief Validates a path whether it is a valid PNG.
   * @param path The path to the PNG file.
   * @return true if the file has a valid PNG signature, false otherwise.
   */
  bool check_valid_png(const std::filesystem::path &path) {
    // PNG signature as defined in PNG specification
    // http://www.libpng.org/pub/png/spec/1.2/PNG-Structure.html
    static constexpr std::array<unsigned char, 8> PNG_SIGNATURE = {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }

    std::array<unsigned char, 8> header;
    file.read(reinterpret_cast<char *>(header.data()), 8);

    if (file.gcount() != 8) {
      return false;
    }

    return header == PNG_SIGNATURE;
  }

  /**
   * @brief Validate app image path.
   */
  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if the extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    if (auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path; std::filesystem::exists(full_image_path)) {
      // Validate PNG signature
      if (!check_valid_png(full_image_path)) {
        BOOST_LOG(warning) << "Invalid PNG file at path ["sv << full_image_path << ']';
        return DEFAULT_APP_IMAGE_PATH;
      }
      return full_image_path.string();
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    if (std::error_code code; !std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // Validate PNG signature
    if (!check_valid_png(app_image_path)) {
      BOOST_LOG(warning) << "Invalid PNG file at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  /**
   * @brief Calculate the SHA-256 digest for a file.
   *
   * @param filename File path whose contents should be hashed.
   * @return Lowercase hexadecimal SHA-256 digest, or std::nullopt on read/hash failure.
   */
  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    std::array<char, 1024 * 16> buf;
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf.data(), buf.size());
      if (!EVP_DigestUpdate(ctx.get(), buf.data(), file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    std::array<unsigned char, SHA256_DIGEST_LENGTH> result;
    if (!EVP_DigestFinal_ex(ctx.get(), result.data(), nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  /**
   * @brief Calculate the CRC-32 checksum for a string.
   *
   * @param input Bytes to include in the checksum.
   * @return CRC-32 value for the input bytes.
   */
  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name) {
    pt::ptree tree;

    try {
      pt::read_json(file_name, tree);

      auto &apps_node = tree.get_child("apps"s);
      auto &env_vars = tree.get_child("env"s);

      auto this_env = boost::this_process::environment();

      for (auto &[name, val] : env_vars) {
        this_env[name] = parse_env_val(this_env, val.get_value<std::string>());
      }

      std::set<std::string, std::less<>> ids;
      std::vector<proc::ctx_t> apps;
      int i = 0;
      for (auto &[_, app_node] : apps_node) {
        proc::ctx_t ctx;

        auto prep_nodes_opt = app_node.get_child_optional("prep-cmd"s);
        auto detached_nodes_opt = app_node.get_child_optional("detached"s);
        auto exclude_global_prep = app_node.get_optional<bool>("exclude-global-prep-cmd"s);
        auto output = app_node.get_optional<std::string>("output"s);
        auto name = parse_env_val(this_env, app_node.get<std::string>("name"s));
        auto cmd = app_node.get_optional<std::string>("cmd"s);
        auto image_path = app_node.get_optional<std::string>("image-path"s);
        auto working_dir = app_node.get_optional<std::string>("working-dir"s);
        auto elevated = app_node.get_optional<bool>("elevated"s);
        auto auto_detach = app_node.get_optional<bool>("auto-detach"s);
        auto wait_all = app_node.get_optional<bool>("wait-all"s);
        auto exit_timeout = app_node.get_optional<int>("exit-timeout"s);

        std::vector<proc::cmd_t> prep_cmds;
        if (!exclude_global_prep.value_or(false)) {
          prep_cmds.reserve(config::sunshine.prep_cmds.size());
          for (auto &prep_cmd : config::sunshine.prep_cmds) {
            auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
            auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);

            prep_cmds.emplace_back(
              std::move(do_cmd),
              std::move(undo_cmd),
              std::move(prep_cmd.elevated)
            );
          }
        }

        if (prep_nodes_opt) {
          auto &prep_nodes = *prep_nodes_opt;

          prep_cmds.reserve(prep_cmds.size() + prep_nodes.size());
          for (auto &[_, prep_node] : prep_nodes) {
            auto do_cmd = prep_node.get_optional<std::string>("do"s);
            auto undo_cmd = prep_node.get_optional<std::string>("undo"s);
            auto elevated = prep_node.get_optional<bool>("elevated");

            prep_cmds.emplace_back(
              parse_env_val(this_env, do_cmd.value_or("")),
              parse_env_val(this_env, undo_cmd.value_or("")),
              std::move(elevated.value_or(false))
            );
          }
        }

        std::vector<std::string> detached;
        if (detached_nodes_opt) {
          auto &detached_nodes = *detached_nodes_opt;

          detached.reserve(detached_nodes.size());
          for (auto &[_, detached_val] : detached_nodes) {
            detached.emplace_back(parse_env_val(this_env, detached_val.get_value<std::string>()));
          }
        }

        if (output) {
          ctx.output = parse_env_val(this_env, *output);
        }

        if (cmd) {
          ctx.cmd = parse_env_val(this_env, *cmd);
        }

        if (working_dir) {
          ctx.working_dir = parse_env_val(this_env, *working_dir);
#ifdef _WIN32
          // The working directory, unlike the command itself, should not be quoted
          // when it contains spaces. Unlike POSIX, Windows forbids quotes in paths,
          // so we can safely strip them all out here to avoid confusing the user.
          boost::erase_all(ctx.working_dir, "\"");
#endif
        }

        if (image_path) {
          ctx.image_path = parse_env_val(this_env, *image_path);
        }

        ctx.elevated = elevated.value_or(false);
        ctx.auto_detach = auto_detach.value_or(true);
        ctx.wait_all = wait_all.value_or(true);
        ctx.exit_timeout = std::chrono::seconds {exit_timeout.value_or(5)};

        auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        } else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        ctx.name = std::move(name);
        ctx.prep_cmds = std::move(prep_cmds);
        ctx.detached = std::move(detached);

        apps.emplace_back(std::move(ctx));
      }

      return proc::proc_t {
        this_env,
        std::move(apps)
      };
    } catch (std::exception &e) {
      BOOST_LOG(error) << e.what();
    }

    return std::nullopt;
  }

  /**
   * @brief Refresh cached platform state from the operating system.
   *
   * This function compares the current last modified time of the file to the time it was last parsed.
   * If the file has been modified since it was last parsed (or if it has never been parsed),
   * it will be re-parsed to update the app list.
   *
   * An optional is used rather than a bare file_time_type because value-initialized
   * file_time_type{} is not a reliable "never parsed" sentinel. On some toolchains
   * (e.g. MSYS2 UCRT64 GCC 16.2), file_time_type::clock::now() compares
   * less-or-equal to file_time_type{}, which would cause the initial parse to be skipped.
   *
   * When an app is currently running, only the cached app configuration list and environment
   * are updated — active process state (_app_id, _process, _process_group, etc.) is preserved
   * so that the in-flight streaming session can still be terminated properly.
   */
  void refresh(const std::string &file_name) {
    static std::optional<std::filesystem::file_time_type> last_apps_file_update;  ///< Timestamp of the last successful apps.json parse, or nullopt if never parsed.

    std::error_code ec;
    auto current_time = std::filesystem::last_write_time(file_name, ec);

    // Only skip parsing when we have a known-good timestamp and the file hasn't changed
    if (last_apps_file_update && !ec && current_time == *last_apps_file_update) {
      return;
    }

    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      proc.update_apps_and_env(std::move(*proc_opt));
      last_apps_file_update = current_time;
    }
  }
}  // namespace proc
