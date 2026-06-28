#include <cerrno>
#include <corio/detail/linux/io_uring.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace corio::detail {
namespace {

[[noreturn]] void throw_uring_err(int neg_err, const char* what) {
  throw std::system_error(-neg_err, std::generic_category(), what);
}

[[noreturn]] void throw_errno(int err, const char* what) {
  throw std::system_error(err, std::generic_category(), what);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

URingPoller::URingPoller() {
  if (const int ret = io_uring_queue_init(kQueueDepth, &ring_, 0); ret < 0) {
    throw_uring_err(ret, "io_uring_queue_init");
  }
  ring_initialized_ = true;

  wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeup_fd_ == -1) {
    io_uring_queue_exit(&ring_);
    throw_errno(errno, "eventfd");
  }

  // Prime the wakeup fd in the ring so the first wake() is seen.
  rearm_wakeup();
  io_uring_submit(&ring_);
}

URingPoller::~URingPoller() noexcept {
  if (wakeup_fd_ != -1) {
    close(wakeup_fd_);
  }
  if (ring_initialized_) {
    io_uring_queue_exit(&ring_);
  }
}

// ---------------------------------------------------------------------------
// Poller interface
// ---------------------------------------------------------------------------

void URingPoller::add(int fd, IOEvent events, void* user) {
  if (registered_.contains(fd)) {
    throw std::invalid_argument("URingPoller::add: fd already registered");
  }
  submit_poll_add(fd, events, user);
  io_uring_submit(&ring_);
  registered_.emplace(fd, FdInfo{events, user});
}

void URingPoller::mod(int fd, IOEvent events, void* user) {
  if (const auto it = registered_.find(fd); it != registered_.end()) {
    // fd is still in the ring -- cancel the pending SQE then re-add.
    submit_poll_remove(fd);
    submit_poll_add(fd, events, user);
    io_uring_submit(&ring_);
    it->second = FdInfo{events, user};
  } else {
    // fd has already fired (one-shot CQE was consumed) -- just re-add.
    submit_poll_add(fd, events, user);
    io_uring_submit(&ring_);
    registered_.emplace(fd, FdInfo{events, user});
  }
}

void URingPoller::rem(int fd) {
  if (registered_.erase(fd) == 0) {
    return; // already fired; the CQE will be ignored in poll()
  }
  submit_poll_remove(fd);
  io_uring_submit(&ring_);
}

void URingPoller::wake() noexcept {
  constexpr std::uint64_t kOne = 1;
  [[maybe_unused]] auto sz = write(wakeup_fd_, &kOne, sizeof(kOne));
}

std::vector<IOToken> URingPoller::poll(std::chrono::milliseconds timeout) {
  io_uring_submit(&ring_);

  if (!wait_for_first_cqe(timeout)) {
    return {};
  }

  std::vector<IOToken> result;
  bool woke = false;

  io_uring_cqe* cqe = nullptr;
  while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
    consume_cqe(cqe, result, woke);
    io_uring_cqe_seen(&ring_, cqe);
  }

  if (woke) {
    rearm_wakeup();
    io_uring_submit(&ring_);
  }

  return result;
}

bool URingPoller::wait_for_first_cqe(std::chrono::milliseconds timeout) {
  io_uring_cqe* cqe = nullptr;
  if (timeout.count() < 0) {
    for (;;) {
      const int ret = io_uring_wait_cqe(&ring_, &cqe);
      if (ret == -EINTR) {
        continue;
      }
      if (ret < 0) {
        throw_uring_err(ret, "io_uring_wait_cqe");
      }
      return true;
    }
  }

  __kernel_timespec ts{};
  ts.tv_sec = timeout.count() / 1000;
  ts.tv_nsec = (timeout.count() % 1000) * 1'000'000LL;
  for (;;) {
    const int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
    if (ret == -EINTR) {
      continue;
    }
    if (ret == -ETIME) {
      return false;
    }
    if (ret < 0) {
      throw_uring_err(ret, "io_uring_wait_cqe_timeout");
    }
    return true;
  }
}

void URingPoller::consume_cqe(const io_uring_cqe* cqe, std::vector<IOToken>& result, bool& woke) {
  const auto data = io_uring_cqe_get_data64(cqe);
  const auto op = decode_op(data);
  const auto fd = decode_fd(data);

  if (op == Op::kWakeup) {
    std::uint64_t val = 0;
    [[maybe_unused]] auto sz = read(wakeup_fd_, &val, sizeof(val));
    woke = true;
    return;
  }

  if (op != Op::kPollAdd || cqe->res <= 0) {
    return; // kPollRemove completion or cancelled/errored kPollAdd -- ignore
  }

  // Fired kPollAdd: fd is ready. Silently drop if rem() already cancelled it.
  if (registered_.erase(fd) > 0) {
    result.push_back(IOToken{
        .fd = fd,
        .events = from_poll_mask(static_cast<std::uint32_t>(cqe->res)),
        .user = nullptr,
    });
  }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::uint32_t URingPoller::to_poll_mask(IOEvent events) noexcept {
  std::uint32_t mask = 0;
  if (any(events & IOEvent::kR)) {
    mask |= POLLIN;
  }
  if (any(events & IOEvent::kW)) {
    mask |= POLLOUT;
  }
  if (any(events & IOEvent::kErr)) {
    mask |= POLLERR;
  }
  return mask;
}

IOEvent URingPoller::from_poll_mask(std::uint32_t mask) noexcept {
  IOEvent ev{};
  if ((mask & POLLIN) != 0U) {
    ev = ev | IOEvent::kR;
  }
  if ((mask & POLLOUT) != 0U) {
    ev = ev | IOEvent::kW;
  }
  if ((mask & (POLLERR | POLLHUP | POLLRDHUP)) != 0U) {
    ev = ev | IOEvent::kErr;
  }
  return ev;
}

io_uring_sqe* URingPoller::get_sqe() {
  io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (sqe != nullptr) {
    return sqe;
  }
  // SQ full: flush and retry.
  io_uring_submit(&ring_);
  sqe = io_uring_get_sqe(&ring_);
  if (sqe == nullptr) {
    throw std::runtime_error("URingPoller: submission queue exhausted");
  }
  return sqe;
}

void URingPoller::submit_poll_add(int fd, IOEvent events, void* /*user*/) {
  auto* sqe = get_sqe();
  io_uring_prep_poll_add(sqe, fd, to_poll_mask(events));
  io_uring_sqe_set_data64(sqe, encode(Op::kPollAdd, fd));
}

void URingPoller::submit_poll_remove(int fd) {
  auto* sqe = get_sqe();
  io_uring_prep_poll_remove(sqe, encode(Op::kPollAdd, fd));
  io_uring_sqe_set_data64(sqe, encode(Op::kPollRemove, fd));
}

void URingPoller::rearm_wakeup() {
  auto* sqe = get_sqe();
  io_uring_prep_poll_add(sqe, wakeup_fd_, POLLIN);
  io_uring_sqe_set_data64(sqe, encode(Op::kWakeup, wakeup_fd_));
}

} // namespace corio::detail
