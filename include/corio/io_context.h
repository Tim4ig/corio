#pragma once

#include <corio/detail/poller.h>
#include <coroutine>
#include <memory>
#include <queue>
#include <unordered_map>

namespace corio {
/// @brief Single-threaded async I/O event loop.
///
/// IoContext owns a Poller (platform I/O multiplexer) and drives coroutines
/// to completion. Exactly one IoContext runs per OS thread at a time.
///
/// Typical usage:
/// @code
/// auto ctx = corio::make_io_context();
/// auto task = my_root_task(&ctx);
/// ctx.post(task.native_handle());
/// ctx.run();
/// @endcode
///
/// Extension: higher-level libraries (networking, timers) register file
/// descriptors via watch_read() / watch_write() from within co_await.
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
  void run();

  /// @brief Signal the loop to exit after the current coroutine yields.
  ///        Safe to call from within a running coroutine.
  void stop() noexcept;

  /// @brief Schedule @p handle to run on the next loop iteration.
  void post(std::coroutine_handle<> handle);

  /// @brief Register @p handle to be posted when @p fd becomes readable.
  /// @note Only one reader per fd at a time.
  void watch_read(int fd, std::coroutine_handle<> handle);

  /// @brief Register @p handle to be posted when @p fd becomes writable.
  /// @note Only one writer per fd at a time.
  void watch_write(int fd, std::coroutine_handle<> handle);

  /// @brief Remove all I/O interest for @p fd (e.g., before closing it).
  void unwatch(int fd);

  /// @brief Returns the IoContext running on the current thread, or nullptr.
  [[nodiscard]] static IoContext* current() noexcept;

 private:
  struct FdState {
    std::coroutine_handle<> reader;
    std::coroutine_handle<> writer;
  };

  std::unique_ptr<Poller> poller_;
  std::queue<std::coroutine_handle<>> ready_;
  std::unordered_map<int, FdState> fd_states_;
  bool stopped_{false};

  [[nodiscard]] static IOEvent fd_events(const FdState& state) noexcept;
  void process_token(const IOToken& token);
};

/// @brief Create an IoContext backed by the default platform poller.
///
/// On Linux this is epoll(7). Prefer this over constructing IoContext
/// directly to stay platform-agnostic.
[[nodiscard]] IoContext make_io_context();
} // namespace corio
