#pragma once

#include <algorithm>
#include <atomic>
#include <corio/io_context.h>
#include <coroutine>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
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
/// co_await token.wait();   // suspends until canceled
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
    IoContext* ctx{nullptr};
    std::coroutine_handle<> handle;
    bool registered{false};

    explicit WaitAwaitable(std::shared_ptr<CancellationSource::State> state_in) noexcept
      : state(std::move(state_in)) {
    }

    ~WaitAwaitable() {
      unregister();
    }

    WaitAwaitable(const WaitAwaitable&) = delete;
    WaitAwaitable& operator=(const WaitAwaitable&) = delete;

    WaitAwaitable(WaitAwaitable&& other) noexcept
      : state(std::move(other.state))
      , ctx(std::exchange(other.ctx, nullptr))
      , handle(std::exchange(other.handle, {}))
      , registered(std::exchange(other.registered, false)) {
    }

    WaitAwaitable& operator=(WaitAwaitable&& other) noexcept {
      if (this != &other) {
        unregister();
        state = std::move(other.state);
        ctx = std::exchange(other.ctx, nullptr);
        handle = std::exchange(other.handle, {});
        registered = std::exchange(other.registered, false);
      }
      return *this;
    }

    [[nodiscard]] bool await_ready() const noexcept {
      return state->cancelled.load(std::memory_order_acquire);
    }

    void await_suspend(std::coroutine_handle<> coroutine) {
      ctx = IoContext::current();
      if (ctx == nullptr) {
        throw std::logic_error("corio::CancellationToken::wait requires a running IoContext");
      }

      std::scoped_lock lock(state->mutex);
      if (state->cancelled.load(std::memory_order_acquire)) {
        // became canceled between await_ready and await_suspend
        ctx->post(coroutine);
        return;
      }
      handle = coroutine;
      registered = true;
      state->waiters.emplace_back(ctx, handle);
    }

    void await_resume() noexcept {
      registered = false;
    }

   private:
    void unregister() noexcept {
      if (!registered || !state) {
        return;
      }

      std::scoped_lock lock(state->mutex);
      const auto waiter = std::pair{ctx, handle};
      auto& waiters = state->waiters;
      waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter), waiters.end());
      registered = false;
    }
  };

  [[nodiscard]] WaitAwaitable wait() const {
    if (!state_) {
      throw std::logic_error("corio::CancellationToken::wait called on an empty token");
    }
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
    std::scoped_lock lock(state_->mutex);
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
