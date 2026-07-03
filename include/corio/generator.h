#pragma once

#include <corio/error.h>
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace corio {
template <typename T> class Generator;
namespace detail {
template <typename T> struct GeneratorPromise {
  std::optional<T> value;
  std::coroutine_handle<> consumer;
  std::exception_ptr exception;

  Generator<T> get_return_object() noexcept;

  static std::suspend_always initial_suspend() noexcept {
    return {};
  }

  // On co_yield: stash the value, resume consumer via symmetric transfer.
  struct YieldAwaiter {
    std::coroutine_handle<> consumer;

    [[nodiscard]] static bool await_ready() noexcept {
      return false;
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> /*self*/) const noexcept {
      return consumer;
    }

    static void await_resume() noexcept {
    }
  };

  YieldAwaiter yield_value(T val) noexcept {
    value.emplace(std::move(val));
    return YieldAwaiter{consumer};
  }

  // On co_return / end-of-body: resume consumer with done signal.
  struct FinalAwaiter {
    static bool await_ready() noexcept {
      return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<GeneratorPromise> self) noexcept {
      auto cont = self.promise().consumer;
      return cont ? cont : std::noop_coroutine();
    }

    static void await_resume() noexcept {
    }
  };

  static FinalAwaiter final_suspend() noexcept {
    return {};
  }

  static void return_void() noexcept {
  }

  void unhandled_exception() noexcept {
    exception = std::current_exception();
  }
};
} // namespace detail
/// @brief Lazy async sequence producer.
///
/// A Generator<T> coroutine uses co_yield to produce values one at a time.
/// The consumer drives it with co_await gen.next(), which returns
/// std::optional<T>: a value while the generator is alive, nullopt when done.
///
/// @code
/// Generator<int> count(int n) {
///   for (auto i = 0; i < n; ++i) co_yield i;
/// }
///
/// Task<> consume() {
///  auto gen = count(5);
///  while (auto v = co_await gen.next()) {
///    use(*v);
///   }
/// }
/// @endcode
template <typename T> class Generator {
 public:
  using promise_type = detail::GeneratorPromise<T>;
  using Handle = std::coroutine_handle<promise_type>;

  Generator() = default;
  explicit Generator(Handle handle) noexcept
    : handle_(handle) {
  }

  ~Generator() {
    if (handle_) {
      handle_.destroy();
    }
  }

  Generator(const Generator&) = delete;
  Generator& operator=(const Generator&) = delete;

  Generator(Generator&& other) noexcept
    : handle_(std::exchange(other.handle_, {})) {
  }
  Generator& operator=(Generator&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  /// @brief Advance the generator and retrieve the next value.
  /// @return Awaitable that yields std::optional<T>:
  ///         - engaged while the generator is running,
  ///         - nullopt after co_return or end of body.
  struct NextAwaitable {
    Handle gen;

    [[nodiscard]] bool await_ready() const noexcept {
      return !gen || gen.done();
    }

    Handle await_suspend(std::coroutine_handle<> consumer) {
      if (!gen) {
        throw corio::EmptyHandleError("corio::Generator: cannot await an empty generator");
      }
      gen.promise().consumer = consumer;
      return gen;
    }

    std::optional<T> await_resume() {
      if (!gen) {
        throw corio::EmptyHandleError("corio::Generator: cannot resume an empty generator");
      }
      auto& promise = gen.promise();
      if (promise.exception) {
        std::rethrow_exception(promise.exception);
      }
      if (gen.done()) {
        return std::nullopt;
      }
      return std::move(promise.value);
    }
  };

  [[nodiscard]] NextAwaitable next() noexcept {
    return NextAwaitable{handle_};
  }

 private:
  Handle handle_;
};

namespace detail {
template <typename T> Generator<T> GeneratorPromise<T>::get_return_object() noexcept {
  return Generator<T>{std::coroutine_handle<GeneratorPromise>::from_promise(*this)};
}
} // namespace detail
} // namespace corio
