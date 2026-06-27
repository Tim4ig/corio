#pragma once

#include <corio/io_context.h>
#include <corio/task.h>
#include <exception>
#include <optional>
#include <stdexcept>

namespace test {

/// Run a coroutine to completion on a fresh IoContext.
/// The coroutine receives an IoContext* and must call ctx->stop() before returning.
template <typename Factory> void run(Factory&& factory) {
  auto ctx = corio::make_io_context();
  auto task = factory(&ctx);
  ctx.post(task.native_handle());
  ctx.run();
}

/// Run a coroutine that returns T; stop() is called automatically.
template <typename T> T run_task(corio::Task<T> task) {
  auto ctx = corio::make_io_context();
  std::optional<T> result;
  std::exception_ptr exc;

  // Named lambda: closure must outlive the coroutine frame (GCC stores &this, not a copy).
  auto fn = [&]() -> corio::Task<> {
    try {
      result = co_await std::move(task);
    } catch (...) {
      exc = std::current_exception();
    }
    ctx.stop();
  };
  auto wrapper = fn();
  ctx.post(wrapper.native_handle());
  ctx.run();

  if (exc) {
    std::rethrow_exception(exc);
  }
  return std::move(*result);
}

template <> inline void run_task<void>(corio::Task<void> task) {
  auto ctx = corio::make_io_context();
  std::exception_ptr exc;

  auto fn = [&]() -> corio::Task<> {
    try {
      co_await std::move(task);
    } catch (...) {
      exc = std::current_exception();
    }
    ctx.stop();
  };
  auto wrapper = fn();
  ctx.post(wrapper.native_handle());
  ctx.run();

  if (exc) {
    std::rethrow_exception(exc);
  }
}

} // namespace test
