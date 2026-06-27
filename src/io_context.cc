#include <corio/detail/platform.h>
#include <corio/io_context.h>
#include <stdexcept>

namespace corio {
namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local IoContext* tls_current_ctx = nullptr;
} // namespace

IoContext::IoContext(std::unique_ptr<Poller> poller)
  : poller_(std::move(poller)) {
}

void IoContext::run() {
  auto* const prev = tls_current_ctx;
  tls_current_ctx = this;

  for (;;) {
    while (!ready_.empty()) {
      auto handle = ready_.front();
      ready_.pop();
      handle.resume();
    }

    if (stopped_) {
      break;
    }

    for (const auto& token : poller_->poll(std::chrono::milliseconds{-1})) {
      process_token(token);
    }
  }

  tls_current_ctx = prev;
}

void IoContext::stop() noexcept {
  stopped_ = true;
  poller_->wake();
}

void IoContext::post(std::coroutine_handle<> handle) {
  ready_.push(handle);
}

void IoContext::watch_read(int fd, std::coroutine_handle<> handle) {
  auto [it, inserted] = fd_states_.emplace(fd, FdState{});
  auto& state = it->second;
  if (state.reader) {
    throw std::logic_error("IoContext::watch_read: fd already has a pending reader");
  }
  state.reader = handle;
  const auto events = fd_events(state);
  if (inserted) {
    poller_->add(fd, events, nullptr);
  } else {
    poller_->mod(fd, events, nullptr);
  }
}

void IoContext::watch_write(int fd, std::coroutine_handle<> handle) {
  auto [it, inserted] = fd_states_.emplace(fd, FdState{});
  auto& state = it->second;
  if (state.writer) {
    throw std::logic_error("IoContext::watch_write: fd already has a pending writer");
  }
  state.writer = handle;
  const auto events = fd_events(state);
  if (inserted) {
    poller_->add(fd, events, nullptr);
  } else {
    poller_->mod(fd, events, nullptr);
  }
}

void IoContext::unwatch(int fd) {
  if (fd_states_.erase(fd) > 0) {
    poller_->rem(fd);
  }
}

IoContext* IoContext::current() noexcept {
  return tls_current_ctx;
}

IOEvent IoContext::fd_events(const FdState& state) noexcept {
  IOEvent events{};
  if (state.reader) {
    events = events | IOEvent::kR;
  }
  if (state.writer) {
    events = events | IOEvent::kW;
  }
  return events;
}

void IoContext::process_token(const IOToken& token) {
  const auto it = fd_states_.find(token.fd);
  if (it == fd_states_.end()) {
    return;
  }

  auto& state = it->second;

  if (any(token.events & (IOEvent::kR | IOEvent::kErr)) && state.reader) {
    ready_.push(state.reader);
    state.reader = {};
  }
  if (any(token.events & (IOEvent::kW | IOEvent::kErr)) && state.writer) {
    ready_.push(state.writer);
    state.writer = {};
  }

  if (const auto remaining = fd_events(state); !any(remaining)) {
    poller_->rem(token.fd);
    fd_states_.erase(it);
  } else {
    poller_->mod(token.fd, remaining, nullptr);
  }
}

IoContext make_io_context() {
  return IoContext{std::make_unique<detail::TargetPoller>()};
}
} // namespace corio
