#pragma once

// Forward-declare IoContext to avoid a circular include chain:
//   io_context.h -> platform.h -> linux/timer.h -> io_context.h
namespace corio {
class IoContext;
}

#include <chrono>
#include <coroutine>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>

namespace corio::detail {
/// timerfd(2) backed sleep awaitable for Linux.
/// Created by async_sleep(); not meant to be used directly.
class TimerfdSleepAwaitable {
 public:
  explicit TimerfdSleepAwaitable(std::chrono::nanoseconds dur) noexcept
    : duration_(dur) {
  }

  ~TimerfdSleepAwaitable() {
    if (fd_ != -1) {
      close(fd_);
    }
  }

  TimerfdSleepAwaitable(const TimerfdSleepAwaitable&) = delete;
  TimerfdSleepAwaitable& operator=(const TimerfdSleepAwaitable&) = delete;

  TimerfdSleepAwaitable(TimerfdSleepAwaitable&&) = delete;
  TimerfdSleepAwaitable& operator=(TimerfdSleepAwaitable&&) = delete;

  [[nodiscard]] bool await_ready() const noexcept {
    return duration_.count() <= 0;
  }

  void await_suspend(std::coroutine_handle<> handle);

  void await_resume() noexcept {
    uint64_t val = 0;
    [[maybe_unused]] auto bytes_read = read(fd_, &val, sizeof(val));
    close(fd_);
    fd_ = -1;
  }

 private:
  std::chrono::nanoseconds duration_;
  int fd_{-1};
};

} // namespace corio::detail

// Included here (after the class body) to break the circular dependency:
// TimerfdSleepAwaitable::await_suspend needs IoContext::current().
#include <corio/io_context.h>

inline void corio::detail::TimerfdSleepAwaitable::await_suspend(std::coroutine_handle<> handle) {
  fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd_ == -1) {
    throw std::system_error(errno, std::generic_category(), "timerfd_create");
  }

  itimerspec spec{};
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration_);
  spec.it_value.tv_sec = secs.count();
  spec.it_value.tv_nsec = (duration_ - secs).count();

  if (timerfd_settime(fd_, 0, &spec, nullptr) == -1) {
    const auto err = errno;
    close(fd_);
    fd_ = -1;
    throw std::system_error(err, std::generic_category(), "timerfd_settime");
  }

  IoContext::current()->watch_read(fd_, handle);
}
