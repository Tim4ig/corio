#pragma once

#include <corio/detail/thread_pool.h>
#include <thread>
#include <utility>

namespace corio::detail {
/// @brief Default ThreadPool: one raw, detached std::thread per job.
///
/// No reuse, no bound on concurrency, no queueing -- a placeholder
/// sufficient for occasional blocking calls. Install a real pool via
/// IoContext::set_thread_pool() for workloads where thread-per-job is too
/// expensive.
class RawThreadPool : public ThreadPool {
 public:
  void submit(std::function<void()> job) override {
    std::thread(std::move(job)).detach();
  }
};
} // namespace corio::detail
