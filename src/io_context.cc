#include <corio/detail/platform.h>
#include <corio/io_context.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace corio {

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> g_thread_counter{0};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local int tls_thread_id =
    g_thread_counter.fetch_add(1, std::memory_order_relaxed);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local IoContext* tls_current_ctx = nullptr;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local std::shared_ptr<detail::ContextMap> tls_current_context;

} // namespace

IoContext::IoContext(std::unique_ptr<Poller> poller)
    : poller_(std::move(poller))
    , owner_thread_(std::this_thread::get_id()) {}

void IoContext::check_thread() const {
    if (std::this_thread::get_id() != owner_thread_) {
        throw std::logic_error(
            "IoContext: method called from a thread other than the owning thread "
            "(logical id " + std::to_string(tls_thread_id) + ")");
    }
}

IoContext::CtxPtr IoContext::snapshot_context() {
    const auto& cur = tls_current_context;
    if (!cur) { return nullptr; }
    return std::make_shared<detail::ContextMap>(*cur);
}

void IoContext::run() {
    check_thread();
    std::clog << "[corio] thread " << tls_thread_id << ": IoContext::run() start\n";

    auto* const prev_ctx = tls_current_ctx;
    tls_current_ctx = this;

    for (;;) {
        {
            std::lock_guard lock(external_mutex_);
            while (!external_.empty()) {
                ready_.push(std::move(external_.front()));
                external_.pop();
            }
        }

        while (!ready_.empty()) {
            auto [handle, ctx] = std::move(ready_.front());
            ready_.pop();
            tls_current_context = std::move(ctx);
            handle.resume();
            tls_current_context = nullptr;
        }

        if (stopped_.load(std::memory_order_relaxed)) { break; }

        for (const auto& token : poller_->poll(std::chrono::milliseconds{-1})) {
            process_token(token);
        }
    }

    tls_current_ctx = prev_ctx;
    tls_current_context = nullptr;
    std::clog << "[corio] thread " << tls_thread_id << ": IoContext::run() stop\n";
}

void IoContext::stop() noexcept {
    stopped_.store(true, std::memory_order_relaxed);
    poller_->wake();
}

void IoContext::post(std::coroutine_handle<> handle) {
    auto ctx = snapshot_context();
    if (std::this_thread::get_id() == owner_thread_) {
        ready_.emplace(handle, std::move(ctx));
    } else {
        std::lock_guard lock(external_mutex_);
        external_.emplace(handle, std::move(ctx));
        poller_->wake();
    }
}

void IoContext::watch_read(int fd, std::coroutine_handle<> handle) {
    check_thread();
    auto [it, inserted] = fd_states_.emplace(fd, FdState{});
    auto& state = it->second;
    if (state.reader) {
        throw std::logic_error("IoContext::watch_read: fd already has a pending reader");
    }
    state.reader     = handle;
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
        throw std::logic_error("IoContext::watch_write: fd already has a pending writer");
    }
    state.writer     = handle;
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

IoContext* IoContext::current() noexcept {
    return tls_current_ctx;
}

std::shared_ptr<detail::ContextMap>& IoContext::current_context() noexcept {
    return tls_current_context;
}

IOEvent IoContext::fd_events(const FdState& state) noexcept {
    IOEvent events{};
    if (state.reader) { events = events | IOEvent::kR; }
    if (state.writer) { events = events | IOEvent::kW; }
    return events;
}

void IoContext::process_token(const IOToken& token) {
    const auto it = fd_states_.find(token.fd);
    if (it == fd_states_.end()) { return; }

    auto& state = it->second;

    if (any(token.events & (IOEvent::kR | IOEvent::kErr)) && state.reader) {
        ready_.emplace(state.reader, std::move(state.reader_ctx));
        state.reader = {};
    }
    if (any(token.events & (IOEvent::kW | IOEvent::kErr)) && state.writer) {
        ready_.emplace(state.writer, std::move(state.writer_ctx));
        state.writer = {};
    }

    const auto remaining = fd_events(state);
    if (!any(remaining)) {
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
