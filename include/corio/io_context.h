#pragma once

#include <atomic>
#include <chrono>
#include <corio/detail/context.h>
#include <corio/detail/poller.h>
#include <corio/task.h>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace corio {
/// @brief Single-threaded async I/O event loop.
///
/// One IoContext per OS thread. Methods that mutate internal state must be
/// called from the owning thread; thread-safe methods are noted explicitly.
///
/// The IoContext must outlive every task, timer and cancellation waiter that
/// references it (including cross-thread stop()/post() callers).
///
/// @code
/// auto ctx = corio::make_io_context();
/// ctx.spawn(my_root_task(&ctx));
/// ctx.run();
/// @endcode
class IoContext {
 public:
  explicit IoContext(std::unique_ptr<Poller> poller);
  ~IoContext() = default;
  IoContext(const IoContext&) = delete;
  IoContext& operator=(const IoContext&) = delete;
  // Move disabled: IoContext address is stored in thread_local storage.
  IoContext(IoContext&&) = delete;
  IoContext& operator=(IoContext&&) = delete;

  /// @brief Run the event loop until stop() is called.
  ///        Must be called from the owning thread.
  ///
  /// Exceptions escaping resumed coroutines propagate out of run() unless an
  /// error handler is installed (see set_error_handler).
  void run();

  /// @brief Signal the loop to exit. Thread-safe. Permanent: a stopped
  ///        context drains already-ready work on the next run() call but
  ///        never blocks for I/O again.
  void stop() noexcept;

  /// @brief Schedule handle to run on the next loop iteration. Thread-safe.
  void post(std::coroutine_handle<> handle);

  /// @brief Run task as a detached, self-owning coroutine. Thread-safe.
  ///
  /// The wrapper frame owns the task and destroys both on completion; an
  /// exception escaping the task is routed to report_error(). Tasks still
  /// pending when the loop stops are leaked, not resumed.
  void spawn(Task<> task);

  /// @brief Install a handler for exceptions escaping resumed coroutines and
  ///        detached tasks. Passing an empty handler restores the defaults:
  ///        run() rethrows, detached-task errors are logged to stderr.
  ///        The handler itself must not throw.
  ///        Must be called from the owning thread.
  void set_error_handler(std::function<void(std::exception_ptr)> handler);

  /// @brief Route an exception to the installed error handler, or log it to
  ///        stderr when no handler is installed. Never throws.
  void report_error(const std::exception_ptr& error) noexcept;

  /// @brief Register handle to be posted when fd becomes readable.
  ///        Must be called from the owning thread.
  void watch_read(int fd, std::coroutine_handle<> handle);

  /// @brief Register handle to be posted when fd becomes writable.
  ///        Must be called from the owning thread.
  void watch_write(int fd, std::coroutine_handle<> handle);

  /// @brief Remove all I/O interest for fd.
  ///        Must be called from the owning thread.
  void unwatch(int fd);

  /// @brief Remove a pending read interest if it still belongs to handle.
  ///        Must be called from the owning thread.
  /// @return true when the registered read waiter was removed.
  bool cancel_read(int fd, std::coroutine_handle<> handle) noexcept;

  /// @brief Remove a pending write interest if it still belongs to handle.
  ///        Must be called from the owning thread.
  /// @return true when the registered write waiter was removed.
  bool cancel_write(int fd, std::coroutine_handle<> handle) noexcept;

  /// @brief Remove queued (fired but not yet resumed) resumes of handle from
  ///        the ready queues. Must be called from the owning thread.
  ///
  /// Call this before destroying a suspended coroutine whose event may have
  /// fired already; cancel_read()/cancel_write()/cancel_timer() returning
  /// false signals exactly that window.
  /// @return true when at least one queued resume was removed.
  bool cancel_posted(std::coroutine_handle<> handle) noexcept;

  /// @brief Schedule handle to be resumed at deadline (no fd is used).
  ///        Must be called from the owning thread.
  /// @return Timer id for cancel_timer().
  std::uint64_t add_timer(std::chrono::steady_clock::time_point deadline, std::coroutine_handle<> handle);

  /// @brief Cancel a pending timer. Must be called from the owning thread.
  /// @return true when the timer was removed before firing.
  bool cancel_timer(std::uint64_t id) noexcept;

  /// @brief Returns the IoContext running on the current thread, or nullptr.
  [[nodiscard]] static IoContext* current() noexcept;

  /// @brief Returns the active ContextMap for the current coroutine.
  ///        Null outside a running task. Used by ContextVar<T>.
  [[nodiscard]] static std::shared_ptr<detail::ContextMap>& current_context() noexcept;

 private:
  using CtxPtr = std::shared_ptr<detail::ContextMap>;
  using HandleWithCtx = std::pair<std::coroutine_handle<>, CtxPtr>;

  struct FdState {
    std::coroutine_handle<> reader;
    std::coroutine_handle<> writer;
    CtxPtr reader_ctx;
    CtxPtr writer_ctx;
  };

  struct TimerEntry {
    std::chrono::steady_clock::time_point deadline{};
    std::uint64_t id{0};
  };

  struct TimerOrder {
    bool operator()(const TimerEntry& lhs, const TimerEntry& rhs) const noexcept {
      return lhs.deadline > rhs.deadline; // min-heap on deadline
    }
  };

  std::unique_ptr<Poller> poller_;
  std::deque<HandleWithCtx> ready_;
  std::unordered_map<int, FdState> fd_states_;
  // Deadlines live in the heap; cancel_timer() erases from active_timers_
  // and the orphaned heap entry is purged lazily.
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerOrder> timer_heap_;
  std::unordered_map<std::uint64_t, HandleWithCtx> active_timers_;
  std::uint64_t next_timer_id_{0};
  std::function<void(std::exception_ptr)> error_handler_;
  std::atomic<bool> stopped_{false};
  std::thread::id owner_thread_;

  std::mutex external_mutex_;
  std::deque<HandleWithCtx> external_;

  [[nodiscard]] static IOEvent fd_events(const FdState& state) noexcept;
  void process_token(const IOToken& token);
  void check_thread() const;
  bool cancel_interest(int fd, std::coroutine_handle<> handle, bool read) noexcept;
  void drain_external();
  void dispatch_ready();
  void expire_timers();
  /// Purges cancelled heap entries; returns -1 when no timer is pending.
  [[nodiscard]] std::chrono::milliseconds next_poll_timeout();

  /// Snapshot of the current context. Lazy copy-on-write: the map is shared
  /// and ContextVar<T>::set() copies it when it is shared.
  [[nodiscard]] static CtxPtr snapshot_context();
};

/// @brief Create an IoContext backed by the default platform poller.
///
/// On Linux this uses io_uring(7).
[[nodiscard]] IoContext make_io_context();
} // namespace corio
