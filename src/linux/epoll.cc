#include <array>
#include <cerrno>
#include <corio/detail/linux/epoll.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace corio::detail {
namespace {
[[noreturn]] void throw_errno(int err, const char* what) {
  throw std::system_error(err, std::generic_category(), what);
}

[[noreturn]] void throw_errno(const char* what) {
  throw_errno(errno, what);
}

// IOEvent uses a library-internal bitmask (kR=1, kW=2, kErr=4).
// epoll uses different constants (EPOLLIN=1, EPOLLOUT=4, EPOLLERR=8).
// Translate explicitly to prevent silent mapping bugs.

[[nodiscard]] constexpr uint32_t to_epoll(IOEvent events) noexcept {
  uint32_t result = 0;
  if (any(events & IOEvent::kR)) {
    result |= EPOLLIN;
  }
  if (any(events & IOEvent::kW)) {
    result |= EPOLLOUT;
  }
  if (any(events & IOEvent::kErr)) {
    result |= EPOLLERR;
  }
  return result;
}

[[nodiscard]] constexpr IOEvent from_epoll(uint32_t events) noexcept {
  IOEvent result{};
  if ((events & EPOLLIN) != 0U) {
    result = result | IOEvent::kR;
  }
  if ((events & EPOLLOUT) != 0U) {
    result = result | IOEvent::kW;
  }
  if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
    result = result | IOEvent::kErr;
  }
  return result;
}

[[nodiscard]] constexpr epoll_event make_epoll_event(int fd, IOEvent events) noexcept {
  epoll_event ev{};
  ev.events = to_epoll(events);
  ev.data.fd = fd;
  return ev;
}

[[nodiscard]] constexpr int to_epoll_timeout(std::chrono::milliseconds ms) noexcept {
  if (ms.count() < 0) {
    return -1;
  }
  constexpr auto kMax = std::chrono::milliseconds{std::numeric_limits<int>::max()};
  return static_cast<int>(std::min(ms, kMax).count());
}
} // namespace

EPoller::EPoller()
  : epoll_fd_(epoll_create1(EPOLL_CLOEXEC))
  , wakeup_fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (epoll_fd_ == -1) {
    throw_errno("epoll_create1");
  }
  if (wakeup_fd_ == -1) {
    const auto err = errno;
    close(epoll_fd_);
    throw_errno(err, "eventfd");
  }
  auto ev = make_epoll_event(wakeup_fd_, IOEvent::kR);
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) == -1) {
    const auto err = errno;
    close(wakeup_fd_);
    close(epoll_fd_);
    throw_errno(err, "epoll_ctl ADD wakeup_fd");
  }
}

EPoller::~EPoller() noexcept {
  if (wakeup_fd_ != -1) {
    close(wakeup_fd_);
  }
  if (epoll_fd_ != -1) {
    close(epoll_fd_);
  }
}

void EPoller::add(int fd, IOEvent events, void* user) {
  if (tokens_.contains(fd)) {
    throw std::invalid_argument("EPoller::add: fd already registered");
  }
  tokens_.emplace(fd, IOToken{.fd = fd, .events = events, .user = user});
  auto ev = make_epoll_event(fd, events);
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
    const auto err = errno;
    tokens_.erase(fd);
    throw_errno(err, "epoll_ctl ADD");
  }
}

void EPoller::mod(int fd, IOEvent events, void* user) {
  const auto it = tokens_.find(fd);
  if (it == tokens_.end()) {
    throw std::invalid_argument("EPoller::mod: fd not registered");
  }
  auto ev = make_epoll_event(fd, events);
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
    throw_errno("epoll_ctl MOD");
  }
  it->second.events = events;
  it->second.user = user;
}

void EPoller::rem(int fd) {
  const auto it = tokens_.find(fd);
  if (it == tokens_.end()) {
    throw std::invalid_argument("EPoller::rem: fd not registered");
  }
  const auto ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  const auto err = errno;
  tokens_.erase(it);
  if (ret == -1) {
    throw_errno(err, "epoll_ctl DEL");
  }
}

void EPoller::wake() noexcept {
  constexpr uint64_t kVal = 1;
  [[maybe_unused]] auto r = write(wakeup_fd_, &kVal, sizeof(kVal));
}

std::vector<IOToken> EPoller::poll(std::chrono::milliseconds timeout) {
  std::array<epoll_event, kMaxEpoll> ready_events{};
  auto ready = 0;
  for (;;) {
    ready =
        epoll_wait(epoll_fd_, ready_events.data(), static_cast<int>(ready_events.size()), to_epoll_timeout(timeout));
    if (ready != -1) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    throw_errno("epoll_wait");
  }

  std::vector<IOToken> result;
  result.reserve(static_cast<std::size_t>(ready));

  for (const auto& [events, data] : std::span{ready_events}.first(static_cast<std::size_t>(ready))) {
    const auto fd = data.fd;
    if (fd == wakeup_fd_) {
      uint64_t val = 0;
      [[maybe_unused]] auto r = read(wakeup_fd_, &val, sizeof(val));
      continue;
    }
    if (const auto it = tokens_.find(fd); it != tokens_.end()) {
      result.emplace_back(IOToken{
          .fd = fd,
          .events = from_epoll(events),
          .user = it->second.user,
      });
    }
  }
  return result;
}
} // namespace corio::detail
