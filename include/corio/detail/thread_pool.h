#pragma once

#include <functional>

namespace corio::detail {
/// @brief Off-loads work to worker thread(s). Used by to_thread().
///
/// Implementations decide how (and how many) threads run submitted jobs.
/// IoContext owns a shared_ptr<ThreadPool>, defaulting to RawThreadPool;
/// install a different implementation with IoContext::set_thread_pool().
class ThreadPool {
 public:
  ThreadPool() = default;
  virtual ~ThreadPool() noexcept = default;

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  /// @brief Run job on a worker, off the calling thread.
  ///        job must not throw: to_thread() always wraps the user callable
  ///        in a try/catch before it reaches here, but a hand-written job
  ///        submitted directly to a ThreadPool is responsible for its own
  ///        exception safety.
  virtual void submit(std::function<void()> job) = 0;
};
} // namespace corio::detail
