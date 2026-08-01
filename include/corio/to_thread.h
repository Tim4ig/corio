#pragma once

#include <atomic>
#include <corio/detail/thread_pool.h>
#include <corio/error.h>
#include <corio/io_context.h>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace corio {
namespace detail {
template <typename T> struct ToThreadState {
  std::optional<T> result;
  std::exception_ptr exception;
  // First of {job completion, awaiter destruction} to exchange this to true
  // decides the outcome: the job posts the handle iff it got here first: if
  // the awaiter was torn down first (coroutine abandoned mid-wait, e.g. by
  // race()), the job sees `true` and drops the now-dangling handle instead.
  std::atomic<bool> settled{false};
};

template <> struct ToThreadState<void> {
  std::exception_ptr exception;
  std::atomic<bool> settled{false};
};

/// @brief Awaitable returned by to_thread(). Not meant to be used directly.
template <typename F, typename T> class ToThreadAwaitable {
 public:
  explicit ToThreadAwaitable(F fn)
    : fn_(std::move(fn))
    , state_(std::make_shared<ToThreadState<T>>()) {
  }

  ~ToThreadAwaitable() {
    state_->settled.exchange(true, std::memory_order_acq_rel);
  }

  ToThreadAwaitable(const ToThreadAwaitable&) = delete;
  ToThreadAwaitable& operator=(const ToThreadAwaitable&) = delete;

  ToThreadAwaitable(ToThreadAwaitable&&) = delete;
  ToThreadAwaitable& operator=(ToThreadAwaitable&&) = delete;

  [[nodiscard]] static bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle) {
    auto* ctx = IoContext::current();
    if (ctx == nullptr) {
      throw corio::NoContextError("corio::to_thread requires a running IoContext");
    }

    auto state = state_;
    ctx->thread_pool()->submit([fn = std::move(fn_), state, ctx, handle]() mutable {
      try {
        if constexpr (std::is_void_v<T>) {
          fn();
        } else {
          state->result.emplace(fn());
        }
      } catch (...) {
        state->exception = std::current_exception();
      }
      if (!state->settled.exchange(true, std::memory_order_acq_rel)) {
        ctx->post(handle);
      }
    });
  }

  T await_resume() {
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
    if constexpr (std::is_void_v<T>) {
      return;
    } else {
      return std::move(*state_->result);
    }
  }

 private:
  F fn_;
  std::shared_ptr<ToThreadState<T>> state_;
};
} // namespace detail

/// @brief Run fn() on IoContext::current()'s thread pool and resume the
///        caller with its result once done, without blocking the loop.
///
/// The default pool (RawThreadPool) spawns a raw, detached std::thread per
/// call -- no reuse, no bound on concurrency. Install a real pool with
/// IoContext::set_thread_pool() for anything beyond occasional blocking
/// calls (a blocking DB driver, CPU-bound work, legacy sync APIs).
///
/// fn must be nullary; capture whatever arguments it needs. An exception
/// thrown by fn is rethrown at the co_await point, on the IoContext thread.
///
/// @note Must be called from within IoContext::run().
///
/// @code
/// auto ctx = corio::make_io_context();
/// ctx.set_thread_pool(std::make_shared<MyPool>()); // optional
///
/// corio::Task<> worker() {
///   auto hash = co_await corio::to_thread([] { return expensive_hash(); });
/// }
/// @endcode
template <typename F> [[nodiscard]] auto to_thread(F fn) {
  using T = std::invoke_result_t<F>;
  return detail::ToThreadAwaitable<F, T>{std::move(fn)};
}
} // namespace corio
