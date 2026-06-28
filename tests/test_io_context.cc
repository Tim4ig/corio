#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <corio/io_context.h>
#include <corio/task.h>
#include <stdexcept>
#include <thread>

using namespace corio;

TEST_CASE("post() schedules a coroutine and run() executes it", "[io_context]") {
  auto ctx = make_io_context();
  auto ran = false;

  auto task_fn = [&]() -> Task<> {
    ran = true;
    ctx.stop();
    co_return;
  };
  const auto task = task_fn();
  ctx.post(task.native_handle());
  ctx.run();

  CHECK(ran);
}

TEST_CASE("stop() terminates run() even with tasks remaining", "[io_context]") {
  auto ctx = make_io_context();
  auto count = 0;

  auto stopper_fn = [&]() -> Task<> {
    ++count;
    ctx.stop();
    co_return;
  };
  auto never_fn = [&]() -> Task<> {
    ++count;
    co_return;
  };
  const auto stopper = stopper_fn();
  const auto never = never_fn();

  ctx.post(stopper.native_handle());
  ctx.post(never.native_handle());
  ctx.run();

  CHECK(count >= 1);
}

TEST_CASE("current() returns running context inside run()", "[io_context]") {
  auto ctx = make_io_context();
  IoContext* seen = nullptr;

  auto task_fn = [&]() -> Task<> {
    seen = IoContext::current();
    ctx.stop();
    co_return;
  };
  const auto task = task_fn();
  ctx.post(task.native_handle());
  ctx.run();

  CHECK(seen == &ctx);
}

TEST_CASE("current() returns nullptr outside run()", "[io_context]") {
  CHECK(IoContext::current() == nullptr);
}

TEST_CASE("watch_read from wrong thread throws", "[io_context]") {
  auto ctx = make_io_context();
  std::exception_ptr exc;

  std::thread th{[&] {
    try {
      ctx.watch_read(0, {});
    } catch (...) {
      exc = std::current_exception();
    }
  }};
  th.join();

  REQUIRE(exc != nullptr);
  CHECK_THROWS_AS(std::rethrow_exception(exc), std::logic_error);
}

TEST_CASE("post() from another thread wakes the event loop", "[io_context]") {
  auto ctx = make_io_context();
  auto task_ran = false;

  auto waiter_fn = [&]() -> Task<> {
    co_return;
  };
  auto stopper_fn = [&]() -> Task<> {
    task_ran = true;
    ctx.stop();
    co_return;
  };
  const auto waiter = waiter_fn();
  const auto stopper = stopper_fn();

  ctx.post(waiter.native_handle());

  std::thread th{[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    ctx.post(stopper.native_handle());
  }};

  ctx.run();
  th.join();

  CHECK(task_ran);
}

TEST_CASE("run() can be called without throwing", "[io_context]") {
  auto ctx = make_io_context();
  auto task_fn = [&]() -> Task<> {
    ctx.stop();
    co_return;
  };
  const auto task = task_fn();
  ctx.post(task.native_handle());
  CHECK_NOTHROW(ctx.run());
}
