#pragma once

#include <chrono>
#include <corio/io_context.h>
#include <coroutine>
#include <cstdint>
#include <stdexcept>

namespace corio::detail {
/// Deadline-heap backed sleep awaitable; created by async_sleep(), not meant
/// to be used directly. Costs no fd and no syscall per sleep: the deadline
/// lives in the owning IoContext's timer heap and drives the poll timeout.
class SleepAwaitable {
 public:
  explicit SleepAwaitable(std::chrono::nanoseconds dur) noexcept
    : duration_(dur) {
  }

  ~SleepAwaitable() {
    if (!armed_) {
      return;
    }
    // Frame destroyed while suspended: drop the pending timer, or scrub the
    // queued resume when the deadline has already fired.
    if (!ctx_->cancel_timer(id_)) {
      (void)ctx_->cancel_posted(handle_);
    }
  }

  SleepAwaitable(const SleepAwaitable&) = delete;
  SleepAwaitable& operator=(const SleepAwaitable&) = delete;

  SleepAwaitable(SleepAwaitable&&) = delete;
  SleepAwaitable& operator=(SleepAwaitable&&) = delete;

  [[nodiscard]] bool await_ready() const noexcept {
    return duration_.count() <= 0;
  }

  void await_suspend(std::coroutine_handle<> handle) {
    auto* ctx = IoContext::current();
    if (ctx == nullptr) {
      throw std::logic_error("corio::async_sleep requires a running IoContext");
    }

    id_ = ctx->add_timer(std::chrono::steady_clock::now() + duration_, handle);
    ctx_ = ctx;
    handle_ = handle;
    armed_ = true;
  }

  void await_resume() noexcept {
    armed_ = false;
  }

 private:
  std::chrono::nanoseconds duration_;
  IoContext* ctx_{nullptr};
  std::coroutine_handle<> handle_{};
  std::uint64_t id_{0};
  bool armed_{false};
};
} // namespace corio::detail
