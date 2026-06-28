#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <corio/context_var.h>
#include <corio/task.h>
#include <corio/timer.h>
#include <string>

using namespace corio;
using namespace std::chrono_literals;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static ContextVar<int> cv_int;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static ContextVar<std::string> cv_str;

TEST_CASE("ContextVar returns nullopt when not set", "[context_var]") {
  auto fn = []() -> Task<> {
    CHECK_FALSE(cv_int.get().has_value());
    co_return;
  };
  test::run_task<void>(fn());
}

TEST_CASE("ContextVar set/get roundtrip", "[context_var]") {
  auto fn = []() -> Task<> {
    cv_int.set(42);
    CHECK(cv_int.get() == 42);
    cv_str.set("hello");
    CHECK(cv_str.get() == "hello");
    co_return;
  };
  test::run_task<void>(fn());
}

TEST_CASE("ContextVar get_or returns default when not set", "[context_var]") {
  auto fn = []() -> Task<> {
    CHECK(cv_int.get_or(-1) == -1);
    co_return;
  };
  test::run_task<void>(fn());
}

TEST_CASE("ContextVar modifications stay local to the current task", "[context_var]") {
  auto ctx = make_io_context();
  auto seen_a = -1;
  auto seen_b = -1;

  auto task_a_fn = [&]() -> Task<> {
    cv_int.set(100);
    co_await async_sleep(10ms);
    seen_a = cv_int.get_or(-1);
  };
  auto task_b_fn = [&]() -> Task<> {
    cv_int.set(200);
    co_await async_sleep(10ms);
    seen_b = cv_int.get_or(-1);
  };
  const auto task_a = task_a_fn();
  const auto task_b = task_b_fn();

  auto root_fn = [&]() -> Task<> {
    ctx.post(task_a.native_handle());
    ctx.post(task_b.native_handle());
    co_await async_sleep(50ms);
    ctx.stop();
  };
  const auto root = root_fn();

  ctx.post(root.native_handle());
  ctx.run();

  CHECK(seen_a == 100);
  CHECK(seen_b == 200);
}

TEST_CASE("Child task inherits parent context at spawn time", "[context_var]") {
  auto seen_in_child = -1;
  auto child_fn = [&seen_in_child]() -> Task<> {
    seen_in_child = cv_int.get_or(-1);
    co_return;
  };

  test::run([&](IoContext* ctx) -> Task<> {
    cv_int.set(77);
    const auto child = child_fn();
    ctx->post(child.native_handle());
    co_await async_sleep(20ms);
    ctx->stop();
  });

  CHECK(seen_in_child == 77);
}

TEST_CASE("Child modification does not affect parent context", "[context_var]") {
  auto parent_after = -1;
  auto child_fn = []() -> Task<> {
    cv_int.set(999);
    co_return;
  };

  test::run([&](IoContext* ctx) -> Task<> {
    cv_int.set(10);
    const auto child = child_fn();
    ctx->post(child.native_handle());
    co_await async_sleep(20ms);
    parent_after = cv_int.get_or(-1);
    ctx->stop();
  });

  CHECK(parent_after == 10);
}
