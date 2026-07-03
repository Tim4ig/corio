#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <corio/context_var.h>
#include <corio/io_context.h>
#include <corio/task.h>
#include <corio/timer.h>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <string>

using namespace corio;
using namespace std::chrono_literals;

namespace {
/// Coroutine whose unhandled exception escapes resume() -- used to verify the
/// event loop's exception containment. The frame is left suspended at its
/// final point when the exception escapes, so tests destroy it manually.
struct RawTask {
  // NOLINTNEXTLINE(readability-identifier-naming) name mandated by the coroutine protocol
  struct promise_type {
    RawTask get_return_object() noexcept {
      return RawTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    static std::suspend_always initial_suspend() noexcept {
      return {};
    }

    static std::suspend_always final_suspend() noexcept {
      return {};
    }

    static void return_void() noexcept {
    }

    [[noreturn]] static void unhandled_exception() {
      throw; // escape into IoContext::run()
    }
  };

  std::coroutine_handle<> handle;
};

RawTask throwing_coroutine() {
  throw std::runtime_error("boom");
  co_return;
}
} // namespace

TEST_CASE("spawn runs a detached task to completion", "[spawn]") {
  auto ctx = make_io_context();
  auto ran = false;

  auto fn = [&]() -> Task<> {
    co_await async_sleep(1ms);
    ran = true;
    ctx.stop();
  };
  ctx.spawn(fn());
  ctx.run();

  CHECK(ran);
}

TEST_CASE("spawn routes task exceptions to the error handler", "[spawn]") {
  auto ctx = make_io_context();
  std::string message;

  ctx.set_error_handler([&](const std::exception_ptr& error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::runtime_error& ex) {
      message = ex.what();
    } catch (...) { // NOLINT(bugprone-empty-catch) only runtime_error expected
    }
    ctx.stop();
  });

  auto fn = []() -> Task<> {
    throw std::runtime_error("spawned failure");
    co_return;
  };
  ctx.spawn(fn());
  ctx.run();

  CHECK(message == "spawned failure");
}

TEST_CASE("spawn on an empty task throws", "[spawn]") {
  auto ctx = make_io_context();
  CHECK_THROWS_AS(ctx.spawn(Task<>{}), std::logic_error);
}

TEST_CASE("spawned task inherits the spawner's context", "[spawn]") {
  static const ContextVar<int> request_id;
  auto ctx = make_io_context();
  auto seen = 0;

  auto child_fn = [&]() -> Task<> {
    seen = request_id.get_or(-1);
    ctx.stop();
    co_return;
  };
  auto parent_fn = [&]() -> Task<> {
    request_id.set(42);
    ctx.spawn(child_fn());
    co_return;
  };
  ctx.spawn(parent_fn());
  ctx.run();

  CHECK(seen == 42);
}

TEST_CASE("error handler contains exceptions escaping resume()", "[io_context]") {
  auto ctx = make_io_context();
  auto errors = 0;
  auto survived = false;

  ctx.set_error_handler([&](const std::exception_ptr&) {
    ++errors;
  });

  const auto bomb = throwing_coroutine();
  auto after_fn = [&]() -> Task<> {
    survived = true;
    ctx.stop();
    co_return;
  };
  const auto after = after_fn();

  ctx.post(bomb.handle);
  ctx.post(after.native_handle());
  ctx.run();

  CHECK(errors == 1);
  CHECK(survived);
  bomb.handle.destroy();
}

TEST_CASE("without an error handler run() rethrows and restores tls state", "[io_context]") {
  auto ctx = make_io_context();

  const auto bomb = throwing_coroutine();
  ctx.post(bomb.handle);

  CHECK_THROWS_AS(ctx.run(), std::runtime_error);
  CHECK(IoContext::current() == nullptr);
  bomb.handle.destroy();
}
