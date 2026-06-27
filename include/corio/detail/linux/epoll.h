#pragma once

#include <corio/detail/poller.h>
#include <unordered_map>

namespace corio::detail {
/// epoll(7) + eventfd(2) based Poller for Linux.
class EPoller : public Poller {
 public:
  EPoller();
  ~EPoller() noexcept override;

  EPoller(const EPoller&) = delete;
  EPoller& operator=(const EPoller&) = delete;

  EPoller(EPoller&&) = delete;
  EPoller& operator=(EPoller&&) = delete;

  void add(int fd, IOEvent events, void* user) override;
  void mod(int fd, IOEvent events, void* user) override;
  void rem(int fd) override;
  void wake() noexcept override;
  std::vector<IOToken> poll(std::chrono::milliseconds timeout) override;

 private:
  static constexpr auto kMaxEpoll = 1024; // TODO: magic?

  int epoll_fd_{-1};
  int wakeup_fd_{-1};
  std::unordered_map<int, IOToken> tokens_;
};
} // namespace corio::detail
