# CorIO

CorIO is a small C++ coroutine runtime for Linux. It provides the low-level pieces needed to build asynchronous socket
clients, socket servers, and HTTP libraries:

- a single-threaded `IoContext` event loop
- `Task<T>` coroutine results
- `spawn()` for detached, self-owning tasks with exception routing
- `Generator<T>` async sequences
- `gather()` for concurrent task composition (wait for all)
- `race()` for first-to-complete task composition (timeouts)
- `to_thread()` for off-loading blocking calls to a thread pool
- `async_sleep()` backed by a deadline heap (no fd or syscall per sleep)
- cancellation tokens
- task-local context variables
- an io_uring-backed readiness poller with batched submissions

The project is intentionally narrow. It does not implement sockets or HTTP itself. Those layers should be built on top
of `IoContext::watch_read()`, `IoContext::watch_write()`, cancellation, and the task primitives.

## Requirements

- Linux with io_uring support (kernel 5.1+) and liburing
- CMake 3.28 or newer
- Ninja
- A C++23 compiler
- Catch2 3 for tests, either installed or fetched by CMake

## Build

```sh
cmake --preset debug-gcc
cmake --build --preset debug-gcc
ctest --preset debug-gcc
```

Release library build without tests:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCORIO_BUILD_TESTS=OFF
cmake --build build/release
```

Sanitizer build:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

The `asan` preset enables AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer is disabled in the preset
because it is not reliable in ptrace-based execution environments.

## Install

```sh
cmake --install build/release --prefix /usr/local
```

Consumer CMake project:

```cmake
find_package(corio CONFIG REQUIRED)

add_executable(app main.cc)
target_link_libraries(app PRIVATE corio::corio)
```

## Basic Usage

An application owns an `IoContext`, spawns root tasks, and runs the loop.

```cpp
#include <corio/io_context.h>
#include <corio/task.h>
#include <corio/timer.h>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

corio::Task<> worker(corio::IoContext& ctx) {
  co_await corio::async_sleep(100ms);
  std::cout << "done\n";
  ctx.stop();
}

int main() {
  auto ctx = corio::make_io_context();
  ctx.spawn(worker(ctx));
  ctx.run();
}
```

`spawn()` takes ownership of the task: the frame is destroyed automatically on completion, and an escaping exception is
routed to the error handler (see Error Model). For manual scheduling, `ctx.post(task.native_handle())` is still
available; in that mode the `Task` must outlive any posted handle.

## Concurrent Tasks

`gather()` schedules all child tasks on the current `IoContext` and returns their results in argument order.

```cpp
#include <corio/gather.h>
#include <corio/task.h>
#include <corio/timer.h>

#include <chrono>

using namespace std::chrono_literals;

corio::Task<int> fetch_a() {
  co_await corio::async_sleep(10ms);
  co_return 10;
}

corio::Task<int> fetch_b() {
  co_await corio::async_sleep(10ms);
  co_return 20;
}

corio::Task<int> combined() {
  auto [a, b] = co_await corio::gather(fetch_a(), fetch_b());
  co_return a + b;
}
```

If one child throws, `gather()` waits for all children and rethrows the first captured exception.

`race()` schedules all tasks and resumes as soon as the first one finishes, by value or exception. The result is a
`std::variant` tagged by argument position; every task still in flight is abandoned (its coroutine is destroyed while
suspended). This is the composition primitive for per-operation timeouts:

```cpp
#include <corio/race.h>
#include <corio/task.h>
#include <corio/timer.h>

#include <chrono>

using namespace std::chrono_literals;

corio::Task<> deadline(std::chrono::milliseconds dur) {
  co_await corio::async_sleep(dur);
}

corio::Task<> with_timeout(corio::Task<int> op) {
  auto outcome = co_await corio::race(std::move(op), deadline(5s));
  if (outcome.index() == 1) {
    // timed out; op's coroutine was abandoned and its interest (I/O watch,
    // timer, cancellation registration, ...) deregistered by its own
    // awaitable destructors.
  }
}
```

Abandoning a losing task is only safe to the extent every awaitable it suspends on follows corio's cancel-on-destroy
contract (see I/O Integration below) -- true for `async_sleep`, `CancellationToken::wait()`, `to_thread()`, and any I/O
awaitable built the way this README recommends.

## Blocking Calls

`to_thread()` runs a nullary callable on a thread pool and resumes the caller with its result, without blocking the
`IoContext`. The default pool (`RawThreadPool`) spawns one raw, detached `std::thread` per call -- no reuse, no bound on
concurrency, just enough to keep an occasional blocking call (a sync DB driver, a legacy API) off the reactor thread.

```cpp
#include <corio/task.h>
#include <corio/to_thread.h>

corio::Task<std::string> hash_file(std::string path) {
  co_return co_await corio::to_thread([path = std::move(path)] {
    return expensive_blocking_hash(path); // runs off the IoContext thread
  });
}
```

Install a real pool by implementing `corio::detail::ThreadPool` (a single `submit(std::function<void()>)`) and calling
`ctx.set_thread_pool(pool)` before any `to_thread()` call that should observe it:

```cpp
class MyPool : public corio::detail::ThreadPool {
 public:
  void submit(std::function<void()> job) override {
    // enqueue job on a bounded worker pool instead of spawning a thread
  }
};

auto ctx = corio::make_io_context();
ctx.set_thread_pool(std::make_shared<MyPool>());
```

An exception thrown by the callable is rethrown at the `co_await` point, on the `IoContext` thread. If the awaiting
coroutine is abandoned (e.g. it lost a `race()`) before the job finishes, the job detects this and drops its resume
instead of touching the destroyed frame; the pool itself has no way to interrupt a callable already running.

## Cancellation

`CancellationSource` owns a signal. `CancellationToken` is a read-only view that tasks can await.

```cpp
#include <corio/cancellation.h>
#include <corio/task.h>

corio::Task<> wait_for_stop(corio::CancellationToken token) {
  co_await token.wait();
}

void request_stop(corio::CancellationSource& source) {
  source.request_cancellation();
}
```

Cancellation waiters unregister themselves if the waiting task is destroyed before cancellation is requested, including
the window where cancellation already fired but the resume is still queued.

## Task-Local Context

`ContextVar<T>` stores per-task values. Child tasks inherit a snapshot of the parent context at `IoContext::post()`/
`spawn()` time. Snapshots are copy-on-write: the map is shared until a task calls `set()`.

```cpp
#include <corio/context_var.h>
#include <corio/task.h>

struct RequestContext {
  int request_id;
};

inline corio::ContextVar<RequestContext> request_context;

corio::Task<> handle_request(int id) {
  request_context.set(RequestContext{.request_id = id});

  auto ctx = request_context.get();
  if (ctx) {
    // use ctx->request_id
  }

  co_return;
}
```

## I/O Integration

`IoContext` exposes readiness registration for file descriptors:

```cpp
ctx.watch_read(fd, coroutine);
ctx.watch_write(fd, coroutine);
ctx.cancel_read(fd, coroutine);
ctx.cancel_write(fd, coroutine);
ctx.cancel_posted(coroutine);
ctx.unwatch(fd);
```

These calls are the intended base for socket operations. A socket wrapper should:

- set file descriptors to non-blocking mode
- attempt the syscall first
- wait for read or write readiness only on `EAGAIN` or `EWOULDBLOCK`
- unregister pending readiness on operation cancellation or destruction
- close and unwatch file descriptors deterministically

The canonical awaitable destructor pattern (see `tests/test_socket.cc`):

```cpp
~WaitReadable() {
  if (registered && !ctx->cancel_read(fd, handle)) {
    // The event already fired; remove the queued resume so the destroyed
    // frame is never resumed.
    (void)ctx->cancel_posted(handle);
  }
}
```

A waiter whose fd fails to arm (for example the fd was already closed) is woken with an error indication instead of
hanging; the following syscall retry reports the real `errno`.

**Contract: always `unwatch()`/cancel before `close()`.** Closing a watched fd frees its number for reuse; a
subsequently created file (by you, the ring, or a library) can receive the same number, and the pending readiness
registration would silently watch the wrong file. This is inherent to fd-based readiness APIs; CorIO's internal
generation counters protect its own bookkeeping but cannot detect number reuse.

`IoContext` is single-threaded. Methods that mutate fd state must be called from the owning thread unless the method
documentation explicitly says it is thread-safe. `post()`, `spawn()`, and `stop()` are thread-safe. `stop()` is
permanent: a stopped context never blocks for I/O again. The `IoContext` must outlive every task, timer, cancellation
waiter, and cross-thread `stop()`/`post()` caller that references it.

## Error Model

Misuse is reported with `std::logic_error` where practical:

- awaiting or resuming an empty `Task`, or spawning one
- awaiting an empty `Generator`
- waiting on a moved-from `CancellationToken`
- using `async_sleep()` or `gather()` outside a running `IoContext`

System call failures are reported with `std::system_error`.

Runtime error routing is controlled by `set_error_handler()`:

- With no handler installed, any exception (whether from a directly posted coroutine or from a `spawn()`ed task)
  propagates out of `run()`.
- With a handler installed, both are delivered to the handler and the loop keeps running, so one failing connection
  cannot take down the reactor.

```cpp
ctx.set_error_handler([](std::exception_ptr err) {
  // log it; must not throw
});
```

## Project Status

CorIO is a runtime foundation, not a complete networking stack. The current scope is suitable for implementing
higher-level socket and HTTP libraries after defining their ownership, timeout, cancellation, and close semantics on top
of the primitives in this repository.

Planned after the IO layer exists: multishot poll (`IORING_POLL_ADD_MULTI`) with persistent interest registration, which
changes the current one-shot watch contract and is therefore deferred until the IO layer's needs are concrete.
