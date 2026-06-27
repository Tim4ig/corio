#pragma once

#include <corio/detail/context.h>
#include <corio/detail/poller.h>

#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>

namespace corio {

/// @brief Single-threaded async I/O event loop.
///
/// One IoContext per OS thread. Methods that mutate internal state must be
/// called from the owning thread; thread-safe methods are noted explicitly.
///
/// @code
/// auto ctx = corio::make_io_context();
/// auto task = my_root_task(&ctx);
/// ctx.post(task.native_handle());
/// ctx.run();
/// @endcode
class IoContext {
public:
    explicit IoContext(std::unique_ptr<Poller> poller);
    ~IoContext() = default;
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    // Move disabled: IoContext address is stored in thread_local storage.
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    /// @brief Run the event loop until stop() is called.
    ///        Must be called from the owning thread.
    void run();

    /// @brief Signal the loop to exit. Thread-safe.
    void stop() noexcept;

    /// @brief Schedule handle to run on the next loop iteration. Thread-safe.
    void post(std::coroutine_handle<> handle);

    /// @brief Register handle to be posted when fd becomes readable.
    ///        Must be called from the owning thread.
    void watch_read(int fd, std::coroutine_handle<> handle);

    /// @brief Register handle to be posted when fd becomes writable.
    ///        Must be called from the owning thread.
    void watch_write(int fd, std::coroutine_handle<> handle);

    /// @brief Remove all I/O interest for fd.
    ///        Must be called from the owning thread.
    void unwatch(int fd);

    /// @brief Returns the IoContext running on the current thread, or nullptr.
    [[nodiscard]] static IoContext* current() noexcept;

    /// @brief Returns the active ContextMap for the current coroutine.
    ///        Null outside a running task. Used by ContextVar<T>.
    [[nodiscard]] static std::shared_ptr<detail::ContextMap>& current_context() noexcept;

private:
    using CtxPtr = std::shared_ptr<detail::ContextMap>;
    using HandleWithCtx = std::pair<std::coroutine_handle<>, CtxPtr>;

    struct FdState {
        std::coroutine_handle<> reader{};
        std::coroutine_handle<> writer{};
        CtxPtr reader_ctx;
        CtxPtr writer_ctx;
    };

    std::unique_ptr<Poller>             poller_;
    std::queue<HandleWithCtx>           ready_;
    std::unordered_map<int, FdState>    fd_states_;
    std::atomic<bool>                   stopped_{false};
    std::thread::id                     owner_thread_;

    std::mutex                          external_mutex_;
    std::queue<HandleWithCtx>           external_;

    [[nodiscard]] static IOEvent fd_events(const FdState& state) noexcept;
    void process_token(const IOToken& token);
    void check_thread() const;

    /// Snapshot of current context (copy-on-write: each task gets its own).
    [[nodiscard]] static CtxPtr snapshot_context();
};

/// @brief Create an IoContext backed by the default platform poller.
///
/// On Linux this uses epoll(7).
[[nodiscard]] IoContext make_io_context();

} // namespace corio
