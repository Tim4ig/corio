#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <corio/race.h>
#include <corio/task.h>
#include <corio/timer.h>
#include <stdexcept>
#include <variant>

using namespace corio;
using namespace std::chrono_literals;

namespace {
Task<int> immediate(int val) {
  co_return val;
}

Task<int> sleep_then(std::chrono::milliseconds dur, int val) {
  co_await async_sleep(dur);
  co_return val;
}

Task<> sleep_void(std::chrono::milliseconds dur) {
  co_await async_sleep(dur);
}

Task<int> throws_int() {
  throw std::runtime_error("child error");
}
} // namespace

TEST_CASE("race resolves to the first task to finish", "[race]") {
  auto outcome = test::run_task<std::variant<int, int>>([]() -> Task<std::variant<int, int>> {
    co_return co_await race(immediate(1), sleep_then(50ms, 2));
  }());

  REQUIRE(outcome.index() == 0);
  CHECK(std::get<0>(outcome) == 1);
}

TEST_CASE("race picks the fastest of several delayed tasks", "[race]") {
  auto outcome = test::run_task<std::variant<int, int, int>>([]() -> Task<std::variant<int, int, int>> {
    co_return co_await race(sleep_then(50ms, 1), sleep_then(5ms, 2), sleep_then(50ms, 3));
  }());

  REQUIRE(outcome.index() == 1);
  CHECK(std::get<1>(outcome) == 2);
}

TEST_CASE("race composes a timeout: operation wins", "[race]") {
  auto outcome = test::run_task<std::variant<int, std::monostate>>([]() -> Task<std::variant<int, std::monostate>> {
    co_return co_await race(immediate(42), sleep_void(50ms));
  }());

  REQUIRE(outcome.index() == 0);
  CHECK(std::get<0>(outcome) == 42);
}

TEST_CASE("race composes a timeout: deadline wins and abandons the operation", "[race]") {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();

  auto outcome = test::run_task<std::variant<int, std::monostate>>([]() -> Task<std::variant<int, std::monostate>> {
    co_return co_await race(sleep_then(200ms, 1), sleep_void(10ms));
  }());
  const auto elapsed = Clock::now() - start;

  REQUIRE(outcome.index() == 1);
  CHECK(elapsed < 100ms); // the losing 200ms sleep was abandoned, not awaited out
}

TEST_CASE("race propagates the winning exception", "[race]") {
  auto run = [] {
    using R = std::variant<int, int>;
    test::run_task<R>(race(throws_int(), sleep_then(50ms, 1)));
  };
  CHECK_THROWS_AS(run(), std::runtime_error);
}
