/**
 * @file src/thread_safe.h
 * @brief Declarations for thread-safe data structures.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

// local includes
#include "utility.h"

namespace safe {
  /**
   * @brief Thread-safe event value that blocks readers until a value is raised.
   */
  template<class T>
  class event_t {
  public:
    /**
     * @brief Status value stored in the event.
     */
    using status_t = util::optional_t<T>;

    /**
     * @brief Notify waiters that a new event value is available.
     *
     * @param args Arguments forwarded to the callable or parser.
     */
    template<class... Args>
    void raise(Args &&...args) {
      std::lock_guard lg {_lock};
      if (!_continue) {
        return;
      }

      if constexpr (std::is_same_v<std::optional<T>, status_t>) {
        _status = std::make_optional<T>(std::forward<Args>(args)...);
      } else {
        _status = status_t {std::forward<Args>(args)...};
      }

      _cv.notify_all();
    }

    /**
     * @brief Try to remove and return the next queued item without any further wait
     *
     * @return Removed queue item, or empty result when the queue is stopped or empty.
     */
    status_t try_pop() {
      std::lock_guard lg {_lock};
      if (!_status) {
        return util::false_v<status_t>;
      }
      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      return val;
    }

    // pop and view should not be used interchangeably
    /**
     * @brief Remove and return the next queued item, waiting when requested.
     *
     * @return Removed queue item, or empty result when the queue is stopped or empty.
     */
    status_t pop() {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      return val;
    }

    // pop and view should not be used interchangeably
    /**
     * @brief Remove and return the next queued item, waiting when requested.
     *
     * @param delay Maximum wait duration before timing out.
     * @return Removed queue item, or empty result when the queue is stopped or empty.
     */
    template<typename Rep, typename Period>
    status_t pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock};

      if (bool success = _cv.wait_for(ul, delay, [this] {
            return (bool) _status || !_continue;
          });
          !success || !_continue) {
        return util::false_v<status_t>;
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      return val;
    }

    // pop and view should not be used interchangeably
    /**
     * @brief Read the current value without removing it from the queue.
     *
     * @return Optional copy of the front item, or empty status when the queue is closed.
     */
    status_t view() {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    // pop and view should not be used interchangeably
    /**
     * @brief Read the current value without removing it from the queue.
     *
     * @param delay Maximum wait duration before timing out.
     * @return Optional copy of the front item, or empty status after timeout/closure.
     */
    template<class Rep, class Period>
    status_t view(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        if (!_continue || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    /**
     * @brief Inspect whether this event has a value without modifying or consuming it.
     *
     * @return True when a value is available to inspect.
     */
    bool peek() const {
      return _continue && (bool) _status;
    }

    /**
     * @brief Raise the alarm and wake all waiting threads.
     */
    void stop() {
      std::lock_guard lg {_lock};

      _continue = false;

      _cv.notify_all();
    }

    /**
     * @brief Reset the object to its initial empty state.
     */
    void reset() {
      std::lock_guard lg {_lock};

      _continue = true;

      _status = util::false_v<status_t>;
    }

    /**
     * @brief Return whether the queue is still running.
     *
     * @return True while the queue accepts producers and consumers.
     */
    [[nodiscard]] bool running() const {
      return _continue;
    }

  private:
    bool _continue {true};
    status_t _status {util::false_v<status_t>};

    std::condition_variable _cv;
    std::mutex _lock;
  };

  /**
   * @brief Alarm primitive used to wait for and raise shutdown-style signals.
   */
  template<class T>
  class alarm_raw_t {
  public:
    /**
     * @brief Status value carried by the alarm.
     */
    using status_t = util::optional_t<T>;

    /**
     * @brief Mark the alarm as rung and wake waiting threads.
     *
     * @param status Native status code returned by the platform API.
     */
    void ring(const status_t &status) {
      std::lock_guard lg(_lock);

      _status = status;
      _rang = true;
      _cv.notify_one();
    }

    /**
     * @brief Mark the alarm as rung and wake waiting threads.
     *
     * @param status Native status code returned by the platform API.
     */
    void ring(status_t &&status) {
      std::lock_guard lg(_lock);

      _status = std::move(status);
      _rang = true;
      _cv.notify_one();
    }

    /**
     * @brief Wait for for.
     *
     * @param rel_time Rel time.
     * @return Wait result indicating whether the condition was satisfied before timeout.
     */
    template<class Rep, class Period>
    auto wait_for(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this]() {
        return _rang;
      });
    }

    /**
     * @brief Wait for for.
     *
     * @param rel_time Rel time.
     * @param pred Predicate used to decide whether the wait is complete.
     * @return Wait result indicating whether the condition was satisfied before timeout.
     */
    template<class Rep, class Period, class Pred>
    auto wait_for(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this, &pred]() {
        return _rang || pred();
      });
    }

    /**
     * @brief Wait for until.
     *
     * @param rel_time Rel time.
     * @return Wait result indicating whether the condition was satisfied before the deadline.
     */
    template<class Rep, class Period>
    auto wait_until(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this]() {
        return _rang;
      });
    }

    /**
     * @brief Wait for until.
     *
     * @param rel_time Rel time.
     * @param pred Predicate used to decide whether the wait is complete.
     * @return Wait result indicating whether the condition was satisfied before the deadline.
     */
    template<class Rep, class Period, class Pred>
    auto wait_until(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this, &pred]() {
        return _rang || pred();
      });
    }

    /**
     * @brief Block until the event is raised.
     *
     * @return Lock object held after the event is observed.
     */
    auto wait() {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this]() {
        return _rang;
      });
    }

    /**
     * @brief Block until the event is raised and the predicate accepts the state.
     *
     * @param pred Predicate used to decide whether the wait is complete.
     * @return Lock object held after the predicate succeeds.
     */
    template<class Pred>
    auto wait(Pred &&pred) {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this, &pred]() {
        return _rang || pred();
      });
    }

    /**
     * @brief Return or update the current status value.
     *
     * @return Status status.
     */
    const status_t &status() const {
      return _status;
    }

    /**
     * @brief Return or update the current status value.
     *
     * @return Status status.
     */
    status_t &status() {
      return _status;
    }

    /**
     * @brief Reset the object to its initial empty state.
     */
    void reset() {
      _status = status_t {};
      _rang = false;
    }

  private:
    std::mutex _lock;
    std::condition_variable _cv;

    status_t _status {util::false_v<status_t>};
    bool _rang {false};
  };

  /**
   * @brief Shared alarm primitive used for cross-thread shutdown signals.
   */
  template<class T>
  using alarm_t = std::shared_ptr<alarm_raw_t<T>>;

  /**
   * @brief Create a alarm object or message.
   *
   * @return Constructed alarm object.
   */
  template<class T>
  alarm_t<T> make_alarm() {
    return std::make_shared<alarm_raw_t<T>>();
  }

  /**
   * @brief Thread-safe queue with blocking and shutdown-aware consumers.
   */
  template<class T>
  class queue_t {
  public:
    /**
     * @brief Value type stored in the queue.
     */
    using status_t = util::optional_t<T>;

    /**
     * @brief Behavior when raise() is called with the queue already at its bound.
     */
    enum class overflow_policy_e {
      drop_oldest,  ///< Clear the queue and accept the new item (current default behavior)
      reject  ///< Refuse the new item, leaving existing queued items intact
    };

    /**
     * @brief Construct a bounded blocking queue.
     *
     * @param max_elements Maximum number of elements that may be queued.
     * @param overflow Behavior when the queue reaches its maximum size.
     */
    explicit queue_t(std::uint32_t max_elements = 32, overflow_policy_e overflow = overflow_policy_e::drop_oldest):
        _max_elements {max_elements},
        _overflow_policy {overflow} {
    }

    /**
     * @brief Notify waiters that a new event value may be available.
     *
     * @param args Arguments forwarded to the callable or parser.
     * @return True if the item was queued, false if the queue is stopped or the
     *         configured overflow policy rejects the item.
     */
    template<class... Args>
    bool raise(Args &&...args) {
      std::lock_guard ul {_lock};

      if (!_continue) {
        return false;
      }

      if (_queue.size() >= _max_elements) {
        if (_overflow_policy == overflow_policy_e::reject) {
          return false;
        }
        _queue.clear();
      }

      _queue.emplace_back(std::forward<Args>(args)...);
      _cv.notify_all();
      return true;
    }

    /**
     * @brief Inspect the next queued value without popping it.
     *
     * @return True when a value is available to inspect.
     */
    bool peek() {
      return _continue && !_queue.empty();
    }

    /**
     * @brief Remove and return the next queued item, waiting when requested.
     *
     * @param delay Maximum wait duration before timing out.
     * @return Removed queue item, or empty result when the queue is stopped or empty.
     */
    template<class Rep, class Period>
    status_t pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        if (!_continue || _cv.wait_for(ul, delay) == std::cv_status::timeout) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));

      return val;
    }

    /**
     * @brief Remove and return the next queued item, waiting when requested.
     *
     * @return Removed queue item, or empty result when the queue is stopped or empty.
     */
    status_t pop() {
      std::unique_lock ul {_lock};

      if (!_continue) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        _cv.wait(ul);

        if (!_continue) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));

      return val;
    }

    /**
     * @brief Return the wrapped pointer without synchronization checks.
     *
     * @return Mutable queue storage for callers that already hold external synchronization.
     */
    std::vector<T> &unsafe() {
      return _queue;
    }

    /**
     * @brief Shut down the queue and wake blocked consumers.
     */
    void stop() {
      std::lock_guard lg {_lock};

      _continue = false;

      _cv.notify_all();
    }

    /**
     * @brief Return whether the queue is still running.
     *
     * @return True while the queue accepts producers and consumers.
     */
    [[nodiscard]] bool running() const {
      return _continue;
    }

  private:
    bool _continue {true};
    std::uint32_t _max_elements;

    std::mutex _lock;
    std::condition_variable _cv;

    std::vector<T> _queue;
    overflow_policy_e _overflow_policy;
  };

  /**
   * @brief Shared object storage with custom construction and destruction hooks.
   */
  template<class T>
  class shared_t {
  public:
    /**
     * @brief Object type stored in shared protected storage.
     */
    using element_type = T;

    /**
     * @brief Callback signature used to construct the protected object.
     */
    using construct_f = std::function<int(element_type &)>;
    /**
     * @brief Callback signature used to destroy the protected object.
     */
    using destruct_f = std::function<void(element_type &)>;

    /**
     * @brief Pointer wrapper state protected by synchronization.
     */
    struct ptr_t {
      shared_t *owner;  ///< Shared object that owns the protected value.

      ptr_t():
          owner {nullptr} {
      }

      /**
       * @brief Construct a reference bound to a shared protected object.
       *
       * @param owner Shared object that owns this pointer wrapper.
       */
      explicit ptr_t(shared_t *owner):
          owner {owner} {
      }

      /**
       * @brief Move a shared-object reference without locking the protected object.
       *
       * @param ptr Pointer managed by the safe pointer wrapper.
       */
      ptr_t(ptr_t &&ptr) noexcept:
          owner {ptr.owner} {
        ptr.owner = nullptr;
      }

      /**
       * @brief Copy a shared-object reference without locking the protected object.
       *
       * @param ptr Pointer managed by the safe pointer wrapper.
       */
      ptr_t(const ptr_t &ptr) noexcept:
          owner {ptr.owner} {
        if (!owner) {
          return;
        }

        auto tmp = ptr.owner->ref();
        tmp.owner = nullptr;
      }

      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @param ptr Pointer managed by the safe pointer wrapper.
       * @return Reference or value produced by the operator.
       */
      ptr_t &operator=(const ptr_t &ptr) noexcept {
        if (!ptr.owner) {
          release();

          return *this;
        }

        return *this = std::move(*ptr.owner->ref());
      }

      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @param ptr Pointer managed by the safe pointer wrapper.
       * @return Reference or value produced by the operator.
       */
      ptr_t &operator=(ptr_t &&ptr) noexcept {
        if (owner) {
          release();
        }

        std::swap(owner, ptr.owner);

        return *this;
      }

      ~ptr_t() {
        if (owner) {
          release();
        }
      }

      /**
       * @brief Explicitly check whether this shared pointer wrapper owns a live object.
       */
      explicit operator bool() const {
        return owner != nullptr;
      }

      /**
       * @brief Release the COM or platform reference owned by the pointer.
       */
      void release() {
        std::lock_guard lg {owner->_lock};

        if (!--owner->_count) {
          owner->_destruct(*get());
          (*this)->~element_type();
        }

        owner = nullptr;
      }

      /**
       * @brief Return the currently wrapped value or handle.
       *
       * @return Underlying native handle or object pointer.
       */
      element_type *get() const {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }

      /**
       * @brief Access the shared protected object.
       *
       * @return Pointer to the object stored in the shared wrapper.
       */
      element_type *operator->() {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }
    };

    /**
     * @brief Construct a shared protected object with custom close and destruction hooks.
     *
     * @param fc Callable invoked when the shared object is closed.
     * @param fd Callable invoked when the shared object is destroyed.
     */
    template<class FC, class FD>
    shared_t(FC &&fc, FD &&fd):
        _construct {std::forward<FC>(fc)},
        _destruct {std::forward<FD>(fd)} {
    }

    /**
     * @brief Return a shared reference to the protected object.
     *
     * @return Reference handle that keeps the protected object alive.
     */
    [[nodiscard]] ptr_t ref() {
      std::lock_guard lg {_lock};

      if (!_count) {
        new (_object_buf.data()) element_type;
        if (_construct(*reinterpret_cast<element_type *>(_object_buf.data())) != 0) {
          reinterpret_cast<element_type *>(_object_buf.data())->~element_type();
          return ptr_t {nullptr};
        }
      }

      ++_count;

      return ptr_t {this};
    }

  private:
    construct_f _construct;
    destruct_f _destruct;

    std::array<std::uint8_t, sizeof(element_type)> _object_buf;

    std::uint32_t _count = 0;  ///< Number of active references, including dynamically allocated storage.
    std::mutex _lock;
  };

  /**
   * @brief Create a shared object or message.
   *
   * @param fc Callable executed while the shared object is locked.
   * @param fd Native file descriptor to wrap or inspect.
   * @return Constructed shared object.
   */
  template<class T, class F_Construct, class F_Destruct>
  auto make_shared(F_Construct &&fc, F_Destruct &&fd) {
    return shared_t<T> {
      std::forward<F_Construct>(fc),
      std::forward<F_Destruct>(fd)
    };
  }

  /**
   * @brief Boolean alarm used as a simple signal.
   */
  using signal_t = event_t<bool>;

  class mail_raw_t;
  /**
   * @brief Shared mailbox handle used by event and queue wrappers.
   */
  using mail_t = std::shared_ptr<mail_raw_t>;

  /**
   * @brief Run cleanup for completed asynchronous work.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   */
  void cleanup(mail_raw_t *);

  /**
   * @brief Wrapper that posts cleanup work to a mailbox when destroyed.
   */
  template<class T>
  class post_t: public T {
  public:
    /**
     * @brief Explicitly construct a message-posting wrapper around an existing type.
     *
     * @param mail Mailbox used to exchange messages with worker threads.
     * @param args Arguments forwarded to the wrapped type constructor.
     */
    template<class... Args>
    explicit post_t(mail_t mail, Args &&...args):
        T(std::forward<Args>(args)...),
        mail {std::move(mail)} {
    }

    mail_t mail;  ///< Mailbox kept alive until the posted object is destroyed.

    ~post_t() {
      cleanup(mail.get());
    }
  };

  /**
   * @brief Acquire the underlying lock or keyed mutex.
   *
   * @param wp Weak pointer used to test whether the object is still alive.
   * @return Lock guard owning the synchronized object until destruction.
   */
  template<class T>
  inline auto lock(const std::weak_ptr<void> &wp) {
    return std::reinterpret_pointer_cast<typename T::element_type>(wp.lock());
  }

  /**
   * @brief Mailbox backing store for events, queues, and posted cleanup objects.
   */
  class mail_raw_t: public std::enable_shared_from_this<mail_raw_t> {
  public:
    /**
     * @brief Mailbox-backed event wrapper for a typed value.
     */
    template<class T>
    using event_t = std::shared_ptr<post_t<event_t<T>>>;

    /**
     * @brief Mailbox-backed queue wrapper for typed messages.
     */
    template<class T>
    using queue_t = std::shared_ptr<post_t<queue_t<T>>>;

    /**
     * @brief Create a typed event channel or reuse the live channel with the same identifier.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @return Typed event channel associated with the supplied identifier.
     */
    template<class T>
    event_t<T> event(const std::string_view &id) {
      std::lock_guard lg {mutex};

      if (auto it = id_to_post.find(id); it != std::end(id_to_post)) {
        return lock<event_t<T>>(it->second);
      }

      auto post = std::make_shared<typename event_t<T>::element_type>(shared_from_this());
      id_to_post.try_emplace(std::string {id}, post);

      return post;
    }

    /**
     * @brief Create a typed queue channel or reuse the live channel with the same identifier.
     *
     * @param id Identifier for the controller, session, display, or resource.
     * @return Typed queue channel associated with the supplied identifier.
     */
    template<class T>
    queue_t<T> queue(const std::string_view &id) {
      std::lock_guard lg {mutex};

      if (auto it = id_to_post.find(id); it != std::end(id_to_post)) {
        return lock<queue_t<T>>(it->second);
      }

      auto post = std::make_shared<typename queue_t<T>::element_type>(shared_from_this(), 32);
      id_to_post.try_emplace(std::string {id}, post);

      return post;
    }

    /**
     * @brief Run cleanup for completed asynchronous work.
     */
    void cleanup() {
      std::lock_guard lg {mutex};

      for (auto it = std::begin(id_to_post); it != std::end(id_to_post); ++it) {
        const auto &weak = it->second;

        if (weak.expired()) {
          id_to_post.erase(it);

          return;
        }
      }
    }

    std::mutex mutex;  ///< Mutex protecting the map of live posted objects.

    std::map<std::string, std::weak_ptr<void>, std::less<>> id_to_post;  ///< Posted objects keyed by cleanup identifier.
  };

  /**
   * @brief Run cleanup for completed asynchronous work.
   */
  inline void cleanup(mail_raw_t *mail) {
    mail->cleanup();
  }
}  // namespace safe
