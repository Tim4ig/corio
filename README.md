# CorIO

CorIO is a small C++ coroutine runtime for Linux. It provides the low-level pieces needed to build asynchronous socket clients, socket servers, and HTTP libraries:

- a single-threaded `IoContext` event loop
- `Task<T>` coroutine results
- `Generator<T>` async sequences
- `gather()` for concurrent task composition
- timerfd-backed `async_sleep()`
- cancellation tokens
- task-local context variables
- an epoll-backed readiness poller

The project is intentionally narrow. It does not implement sockets or HTTP itself. Those layers should be built on top of `IoContext::watch_read()`, `IoContext::watch_write()`, cancellation, and the task primitives.

## Requirements

- Linux
- CMake 3.28 or newer
- Ninja
- A C++26 compiler
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

The `asan` preset enables AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer is disabled in the preset because it is not reliable in ptrace-based execution environments.

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

An application owns an `IoContext`, creates root tasks, posts their coroutine handles, and runs the loop.

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
  auto task = worker(ctx);

  ctx.post(task.native_handle());
  ctx.run();
}
```

`Task` owns its coroutine frame. A task must outlive any handle posted to `IoContext`.

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

Cancellation waiters unregister themselves if the waiting task is destroyed before cancellation is requested.

## Task-Local Context

`ContextVar<T>` stores per-task values. Child tasks inherit a snapshot of the parent context at `IoContext::post()` time.

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
ctx.unwatch(fd);
```

These calls are the intended base for socket operations. A socket wrapper should:

- set file descriptors to non-blocking mode
- attempt the syscall first
- wait for read or write readiness only on `EAGAIN` or `EWOULDBLOCK`
- unregister pending readiness on operation cancellation or destruction
- close and unwatch file descriptors deterministically

`IoContext` is single-threaded. Methods that mutate fd state must be called from the owning thread unless the method documentation explicitly says it is thread-safe. `post()` and `stop()` are thread-safe.

## Error Model

Misuse is reported with `std::logic_error` where practical:

- awaiting or resuming an empty `Task`
- awaiting an empty `Generator`
- waiting on a moved-from `CancellationToken`
- using `async_sleep()` or `gather()` outside a running `IoContext`

System call failures are reported with `std::system_error`.

## Project Status

CorIO is a runtime foundation, not a complete networking stack. The current scope is suitable for implementing higher-level socket and HTTP libraries after defining their ownership, timeout, cancellation, and close semantics on top of the primitives in this repository.
