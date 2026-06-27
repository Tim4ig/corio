#pragma once

#include <chrono>
#include <corio/detail/token.h>
#include <vector>

namespace corio {
/// @brief Abstract I/O readiness notifier.
///
/// Implementors wrap OS-specific mechanisms (epoll, kqueue, io_uring).
/// IoContext owns one Poller and calls poll() in its event loop.
class Poller {
 public:
  Poller() = default;
  virtual ~Poller() noexcept = default;

  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  Poller(Poller&&) = delete;
  Poller& operator=(Poller&&) = delete;

  virtual void add(int fd, IOEvent events, void* user) = 0;
  virtual void mod(int fd, IOEvent events, void* user) = 0;
  virtual void rem(int fd) = 0;

  /// Unblocks a currently-blocking poll() call from another thread.
  /// Default no-op; override when the implementation supports it.
  virtual void wake() noexcept {
  }

  /// Block until at least one registered fd is ready, or timeout elapses.
  /// @param timeout  Negative value means block indefinitely.
  virtual std::vector<IOToken> poll(std::chrono::milliseconds timeout) = 0;
};
} // namespace corio
