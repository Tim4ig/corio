#pragma once

#include <atomic>
#include <corio/io_context.h>
#include <coroutine>
#include <memory>
#include <mutex>
#include <vector>

namespace corio {

class CancellationToken;
/// @brief Owns a cancellation signal. Call request_cancellation() to fire it.
///
/// CancellationSource and CancellationToken share reference-counted state,
/// so either can outlive the other. Both are cheap to copy.
///
/// @code
/// corio::CancellationSource src;
/// auto token = src.token();
///
/// // in another task or thread:
/// src.request_cancellation();
///
/// // in a coroutine:
/// co_await token.wait();   // suspends until cancelled
/// @endcode
class CancellationSource {
 public:
  CancellationSource()
    : state_(std::make_shared<State>()) {
  }

  [[nodiscard]] CancellationToken token() const noexcept;

  /// @brief Request cancellation. Thread-safe. Idempotent.
  void request_cancellation() const noexcept;

  [[nodiscard]] bool is_cancellation_requested() const noexcept {
    return state_->cancelled.load(std::memory_order_acquire);
  }

  struct State {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::pair<IoContext*, std::coroutine_handle<>>> waiters;
  };

 private:
  std::shared_ptr<State> state_;
};

/// @brief Read-only view of a CancellationSource's signal.
class CancellationToken {
 public:
  [[nodiscard]] bool is_cancellation_requested() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
  }

  /// @brief Awaitable: suspends until cancellation is requested.
  ///        Returns immediately if already canceled.
  struct WaitAwaitable {
    std::shared_ptr<CancellationSource::State> state;

    [[nodiscard]] bool await_ready() const noexcept {
      return state->cancelled.load(std::memory_order_acquire);
    }

    void await_suspend(std::coroutine_handle<> handle) const {
      auto* ctx = IoContext::current();
      std::lock_guard lock(state->mutex);
      if (state->cancelled.load(std::memory_order_acquire)) {
        // became canceled between await_ready and await_suspend
        ctx->post(handle);
        return;
      }
      state->waiters.emplace_back(ctx, handle);
    }

    static void await_resume() noexcept {
    }
  };

  [[nodiscard]] WaitAwaitable wait() const noexcept {
    return WaitAwaitable{state_};
  }

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<CancellationSource::State> state)
    : state_(std::move(state)) {
  }

  std::shared_ptr<CancellationSource::State> state_;
};

inline CancellationToken CancellationSource::token() const noexcept {
  return CancellationToken{state_};
}

inline void CancellationSource::request_cancellation() const noexcept {
  std::vector<std::pair<IoContext*, std::coroutine_handle<>>> waiters;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) {
      return; // already canceled
    }
    waiters = std::move(state_->waiters);
  }

  for (auto& [ctx, handle] : waiters) {
    ctx->post(handle); // thread-safe
  }
}

} // namespace corio
