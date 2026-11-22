/**
 * @file src/video_capture_groups.h
 * @brief Share capture workers only between streams using the same encoder backend.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

// local includes
#include "thread_safe.h"

namespace video {
  struct encoder_t;

  /**
   * @brief Broadcast the latest monitor request to every active capture group.
   */
  class display_switch_state_t {
  public:
    /**
     * @brief Monitor request observed by one capture worker.
     */
    struct request_t {
      std::uint64_t generation = 0;  ///< Changes whenever a monitor request is consumed.
      std::optional<int> index;  ///< Requested monitor index for the current stream epoch.

      /**
       * @brief Apply a switch once to this worker's refreshed display list.
       *
       * @param display_index Selected display, already kept by name across re-enumeration.
       * @param display_count Number of displays in the refreshed list.
       * @param applied_generation Last switch applied by this worker.
       */
      void apply(int &display_index, std::size_t display_count, std::uint64_t &applied_generation) const {
        if (generation == applied_generation || !index.has_value() || display_count == 0) {
          return;
        }
        display_index = std::clamp(*index, 0, static_cast<int>(display_count) - 1);
        applied_generation = generation;
      }
    };

    /**
     * @brief Start a capture group, resetting stale requests between streaming sessions.
     */
    void start_worker() {
      std::lock_guard lock {mutex_};
      if (workers_++ == 0) {
        request_ = {};
      }
    }

    /**
     * @brief Release a stopped capture group's participation in the stream epoch.
     */
    void stop_worker() {
      std::lock_guard lock {mutex_};
      --workers_;
    }

    /**
     * @brief Consume pending requests once and share their generation with every worker.
     *
     * @param event Shared monitor-switch event produced by input handling.
     * @return Most recent request, even when another worker consumed the raw event.
     */
    request_t poll(const safe::mail_raw_t::event_t<int> &event) {
      std::lock_guard lock {mutex_};
      if (const auto request = event->try_pop(); request.has_value()) {
        request_.index = *request;
        ++request_.generation;
      }
      return request_;
    }

  private:
    std::mutex mutex_;  ///< Serializes event consumption and capture group lifetimes.
    std::size_t workers_ = 0;  ///< Number of active backend capture groups.
    request_t request_;  ///< Latest request shared during this streaming epoch.
  };

  /**
   * @brief Maintain independent capture worker lifetimes for encoder backends.
   *
   * @tparam T Capture worker context managed by safe::shared_t.
   */
  template<class T>
  class capture_group_pool_t {
  public:
    using group_t = safe::shared_t<T>;  ///< Reference-counted capture worker storage.
    using ptr_t = typename group_t::ptr_t;  ///< Handle keeping one backend's worker alive.

    /**
     * @brief Configure the callbacks that start and stop each capture worker.
     *
     * @param construct Callback starting a newly referenced backend group.
     * @param destruct Callback stopping a group after its final stream leaves.
     */
    capture_group_pool_t(typename group_t::construct_f construct, typename group_t::destruct_f destruct):
        construct_ {std::move(construct)},
        destruct_ {std::move(destruct)} {
    }

    /**
     * @brief Acquire the capture worker belonging to a stream's encoder backend.
     *
     * @param encoder Stable encoder identity; null does not acquire a group.
     * @return Shared worker handle, or an empty handle when startup fails.
     */
    ptr_t ref(const encoder_t *encoder) {
      if (!encoder) {
        return {};
      }
      std::lock_guard lock {mutex_};
      auto &group = groups_[encoder];
      if (!group) {
        group = std::make_unique<group_t>(construct_, destruct_);
      }
      return group->ref();
    }

  private:
    typename group_t::construct_f construct_;  ///< Worker startup callback.
    typename group_t::destruct_f destruct_;  ///< Worker shutdown callback.
    std::mutex mutex_;  ///< Protects stable group allocation.
    std::map<const encoder_t *, std::unique_ptr<group_t>> groups_;  ///< One group per encoder backend.
  };
}  // namespace video
