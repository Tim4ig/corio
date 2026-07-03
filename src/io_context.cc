#include <algorithm>
#include <corio/detail/platform.h>
#include <corio/error.h>
#include <corio/io_context.h>
#include <exception>
#include <iostream>
#include <string>

namespace corio {
namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic g_thread_counter{0};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local auto tls_thread_id = g_thread_counter.fetch_add(1, std::memory_order_relaxed);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local IoContext* tls_current_ctx = nullptr;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::shared_ptr<detail::ContextMap> tls_current_context;

/// Restores the thread-local slots even when run() unwinds via an exception.
class TlsRunGuard {
 public:
  explicit TlsRunGuard(IoContext* prev) noexcept
    : prev_(prev) {
  }

  ~TlsRunGuard() {
    tls_current_ctx = prev_;
    tls_current_context = nullptr;
  }

  TlsRunGuard(const TlsRunGuard&) = delete;
  TlsRunGuard& operator=(const TlsRunGuard&) = delete;
  TlsRunGuard(TlsRunGuard&&) = delete;
  TlsRunGuard& operator=(TlsRunGuard&&) = delete;

 private:
  IoContext* prev_;
};

/// Self-owning coroutine used by IoContext::spawn(). The frame starts
/// suspended, is resumed once by the loop, and destroys itself on completion
/// (final_suspend is suspend_never).
struct DetachedTask {
  // NOLINTNEXTLINE(readability-identifier-naming) name mandated by the coroutine protocol
  struct promise_type {
    DetachedTask get_return_object() noexcept {
      return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    static std::suspend_always initial_suspend() noexcept {
      return {};
    }

    static std::suspend_never final_suspend() noexcept {
      return {};
    }

    static void return_void() noexcept {
    }

    // The body catches everything; reaching here is a bug in run_detached.
    static void unhandled_exception() noexcept {
      std::terminate();
    }
  };

  std::coroutine_handle<> handle;
};

DetachedTask run_detached(IoContext* ctx, Task<> task) {
  try {
    co_await std::move(task);
  } catch (...) {
    ctx->report_error(std::current_exception());
  }
}
} // namespace

IoContext::IoContext(std::unique_ptr<Poller> poller)
  : poller_(std::move(poller))
  , owner_thread_(std::this_thread::get_id()) {
}

void IoContext::run() {
  check_thread();

  const TlsRunGuard guard{tls_current_ctx};
  tls_current_ctx = this;

  for (;;) {
    drain_external();
    dispatch_ready();

    if (stopped_.load(std::memory_order_relaxed)) {
      break;
    }

    expire_timers();
    if (!ready_.empty()) {
      continue;
    }

    for (const auto& token : poller_->poll(next_poll_timeout())) {
      process_token(token);
    }
  }
}

void IoContext::stop() noexcept {
  stopped_.store(true, std::memory_order_relaxed);
  poller_->wake();
}

void IoContext::post(std::coroutine_handle<> handle) {
  auto ctx = snapshot_context();
  if (std::this_thread::get_id() == owner_thread_) {
    ready_.emplace_back(handle, std::move(ctx));
  } else {
    const std::scoped_lock lock(external_mutex_);
    external_.emplace_back(handle, std::move(ctx));
    poller_->wake();
  }
}

void IoContext::spawn(Task<> task) {
  if (!task.native_handle()) {
    throw corio::EmptyHandleError("IoContext::spawn: cannot spawn an empty task");
  }
  const auto detached = run_detached(this, std::move(task));
  post(detached.handle);
}

void IoContext::set_error_handler(std::function<void(std::exception_ptr)> handler) {
  check_thread();
  error_handler_ = std::move(handler);
}

void IoContext::report_error(const std::exception_ptr& error) noexcept {
  if (error_handler_) {
    try {
      error_handler_(error);
    } catch (...) { // NOLINT(bugprone-empty-catch) handler contract: must not throw
    }
    return;
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& ex) {
    std::cerr << "corio: unhandled exception in detached task: " << ex.what() << '\n';
  } catch (...) {
    std::cerr << "corio: unhandled exception in detached task\n";
  }
}

void IoContext::watch_read(int fd, std::coroutine_handle<> handle) {
  check_thread();
  auto [it, inserted] = fd_states_.emplace(fd, FdState{});
  auto& state = it->second;
  if (state.reader) {
    throw corio::FdConflictError("IoContext::watch_read: fd already has a pending reader");
  }
  state.reader = handle;
  state.reader_ctx = snapshot_context();
  const auto events = fd_events(state);
  if (inserted) {
    poller_->add(fd, events, nullptr);
  } else {
    poller_->mod(fd, events, nullptr);
  }
}

void IoContext::watch_write(int fd, std::coroutine_handle<> handle) {
  check_thread();
  auto [it, inserted] = fd_states_.emplace(fd, FdState{});
  auto& state = it->second;
  if (state.writer) {
    throw corio::FdConflictError("IoContext::watch_write: fd already has a pending writer");
  }
  state.writer = handle;
  state.writer_ctx = snapshot_context();
  const auto events = fd_events(state);
  if (inserted) {
    poller_->add(fd, events, nullptr);
  } else {
    poller_->mod(fd, events, nullptr);
  }
}

void IoContext::unwatch(int fd) {
  check_thread();
  if (fd_states_.erase(fd) > 0) {
    poller_->rem(fd);
  }
}

bool IoContext::cancel_read(int fd, std::coroutine_handle<> handle) noexcept {
  return cancel_interest(fd, handle, true);
}

bool IoContext::cancel_write(int fd, std::coroutine_handle<> handle) noexcept {
  return cancel_interest(fd, handle, false);
}

bool IoContext::cancel_posted(std::coroutine_handle<> handle) noexcept {
  if (std::this_thread::get_id() != owner_thread_) {
    return false;
  }

  try {
    const auto match = [handle](const HandleWithCtx& item) {
      return item.first == handle;
    };
    auto removed = std::erase_if(ready_, match) > 0;
    {
      const std::scoped_lock lock(external_mutex_);
      removed = std::erase_if(external_, match) > 0 || removed;
    }
    return removed;
  } catch (...) {
    return false;
  }
}

std::uint64_t IoContext::add_timer(std::chrono::steady_clock::time_point deadline, std::coroutine_handle<> handle) {
  check_thread();
  const auto id = ++next_timer_id_;
  active_timers_.emplace(id, HandleWithCtx{handle, snapshot_context()});
  timer_heap_.push(TimerEntry{.deadline = deadline, .id = id});
  return id;
}

bool IoContext::cancel_timer(std::uint64_t id) noexcept {
  if (std::this_thread::get_id() != owner_thread_) {
    return false;
  }

  try {
    return active_timers_.erase(id) > 0;
  } catch (...) {
    return false;
  }
}

IoContext* IoContext::current() noexcept {
  return tls_current_ctx;
}

std::shared_ptr<detail::ContextMap>& IoContext::current_context() noexcept {
  return tls_current_context;
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
    ready_.emplace_back(state.reader, std::move(state.reader_ctx));
    state.reader = {};
  }
  if (any(token.events & (IOEvent::kW | IOEvent::kErr)) && state.writer) {
    ready_.emplace_back(state.writer, std::move(state.writer_ctx));
    state.writer = {};
  }

  if (const auto remaining = fd_events(state); !any(remaining)) {
    poller_->rem(token.fd);
    fd_states_.erase(it);
  } else {
    poller_->mod(token.fd, remaining, nullptr);
  }
}

void IoContext::check_thread() const {
  if (std::this_thread::get_id() != owner_thread_) {
    throw corio::ThreadViolationError("IoContext: method called from a thread other than the owning thread "
                                      "(logical id " +
                                      std::to_string(tls_thread_id) + ")");
  }
}

bool IoContext::cancel_interest(int fd, std::coroutine_handle<> handle, bool read) noexcept {
  if (std::this_thread::get_id() != owner_thread_) {
    return false;
  }

  try {
    const auto it = fd_states_.find(fd);
    if (it == fd_states_.end()) {
      return false;
    }

    auto& state = it->second;
    auto& slot = read ? state.reader : state.writer;
    auto& slot_ctx = read ? state.reader_ctx : state.writer_ctx;
    if (slot != handle) {
      return false;
    }

    slot = {};
    slot_ctx.reset();

    if (const auto remaining = fd_events(state); !any(remaining)) {
      poller_->rem(fd);
      fd_states_.erase(it);
    } else {
      poller_->mod(fd, remaining, nullptr);
    }
    return true;
  } catch (...) {
    return false;
  }
}

void IoContext::drain_external() {
  const std::scoped_lock lock(external_mutex_);
  while (!external_.empty()) {
    ready_.push_back(std::move(external_.front()));
    external_.pop_front();
  }
}

void IoContext::dispatch_ready() {
  while (!ready_.empty()) {
    auto [handle, ctx] = std::move(ready_.front());
    ready_.pop_front();
    tls_current_context = std::move(ctx);
    try {
      handle.resume();
    } catch (...) {
      tls_current_context = nullptr;
      if (!error_handler_) {
        throw;
      }
      report_error(std::current_exception());
      continue;
    }
    tls_current_context = nullptr;
  }
}

void IoContext::expire_timers() {
  const auto now = std::chrono::steady_clock::now();
  while (!timer_heap_.empty()) {
    const auto& top = timer_heap_.top();
    const auto it = active_timers_.find(top.id);
    if (it == active_timers_.end()) { // cancelled; purge lazily
      timer_heap_.pop();
      continue;
    }
    if (top.deadline > now) {
      break;
    }
    ready_.push_back(std::move(it->second));
    active_timers_.erase(it);
    timer_heap_.pop();
  }
}

std::chrono::milliseconds IoContext::next_poll_timeout() {
  while (!timer_heap_.empty() && !active_timers_.contains(timer_heap_.top().id)) {
    timer_heap_.pop();
  }
  if (timer_heap_.empty()) {
    return std::chrono::milliseconds{-1};
  }
  const auto until = timer_heap_.top().deadline - std::chrono::steady_clock::now();
  return std::max(std::chrono::milliseconds{0}, std::chrono::ceil<std::chrono::milliseconds>(until));
}

IoContext::CtxPtr IoContext::snapshot_context() {
  // Lazy copy-on-write: share the map; ContextVar<T>::set() copies it when
  // the use count shows it is shared.
  return tls_current_context;
}

IoContext make_io_context() {
  return IoContext{std::make_unique<detail::TargetPoller>()};
}
} // namespace corio
