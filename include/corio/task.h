#pragma once

#include <corio/error.h>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace corio {
template <typename T = void> class Task;
namespace detail {
struct TaskPromiseBase {
  std::coroutine_handle<> continuation;
  std::exception_ptr exception;

  struct FinalAwaiter {
    static bool await_ready() noexcept {
      return false;
    }

    template <typename P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> coro) noexcept {
      auto cont = coro.promise().continuation;
      return cont ? cont : std::noop_coroutine();
    }

    static void await_resume() noexcept {
    }
  };

  static std::suspend_always initial_suspend() noexcept {
    return {};
  }

  static FinalAwaiter final_suspend() noexcept {
    return {};
  }

  void unhandled_exception() noexcept {
    exception = std::current_exception();
  }
};

template <typename T> struct TaskPromise : TaskPromiseBase {
  std::optional<T> result;

  Task<T> get_return_object() noexcept;

  template <typename U> void return_value(U&& val) {
    result.emplace(std::forward<U>(val));
  }
};

template <> struct TaskPromise<void> : TaskPromiseBase {
  Task<> get_return_object() noexcept;

  static void return_void() noexcept {
  }
};
} // namespace detail
/// @brief Eager-lazy coroutine handle. Suspends on first resume; result or
///        exception is delivered at co_await.
///
/// @tparam T Return type. Use @c void (the default) for fire-and-forget tasks.
///
/// Ownership: each Task uniquely owns its coroutine frame. Destroying an
/// un-awaited Task destroys the frame.
template <typename T> class Task {
 public:
  using promise_type = detail::TaskPromise<T>;
  using Handle = std::coroutine_handle<promise_type>;

  Task() = default;
  explicit Task(Handle handle) noexcept
    : handle_(handle) {
  }

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept
    : handle_(std::exchange(other.handle_, {})) {
  }
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return !handle_ || handle_.done();
  }

  Handle await_suspend(std::coroutine_handle<> caller) {
    if (!handle_) {
      throw corio::EmptyHandleError("corio::Task: cannot await an empty task");
    }
    handle_.promise().continuation = caller;
    return handle_;
  }

  T await_resume() {
    if (!handle_) {
      throw corio::EmptyHandleError("corio::Task: cannot resume an empty task");
    }
    auto& promise = handle_.promise();
    if (promise.exception) {
      std::rethrow_exception(promise.exception);
    }
    if constexpr (std::is_void_v<T>) {
      return;
    } else {
      return std::move(*promise.result);
    }
  }

  /// @brief Start the coroutine from a non-coroutine context (event loop
  ///        entry point). Caller is responsible for keeping the Task alive.
  void resume() {
    if (!handle_) {
      throw corio::EmptyHandleError("corio::Task: cannot resume an empty task");
    }
    handle_.resume();
  }

  [[nodiscard]] bool done() const noexcept {
    return !handle_ || handle_.done();
  }

  /// @brief Returns the raw coroutine handle for manual scheduling.
  ///        The Task must outlive any use of the returned handle.
  [[nodiscard]] std::coroutine_handle<> native_handle() const noexcept {
    return handle_;
  }

 private:
  Handle handle_;
};

namespace detail {
template <typename T> Task<T> TaskPromise<T>::get_return_object() noexcept {
  return Task<T>{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

inline Task<> TaskPromise<void>::get_return_object() noexcept {
  return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}
} // namespace detail
} // namespace corio
