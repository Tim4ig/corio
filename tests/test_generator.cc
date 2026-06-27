#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <corio/generator.h>
#include <corio/task.h>
#include <stdexcept>
#include <vector>

using namespace corio;

static Generator<int> fibonacci() {
  int prev = 0;
  int curr = 1;
  for (;;) {
    co_yield prev;
    int next = prev + curr;
    prev = curr;
    curr = next;
  }
}

static Generator<int> finite(int count) {
  for (int i = 0; i < count; ++i) {
    co_yield i;
  }
}

static Generator<int> throws_after(int count) {
  for (int i = 0; i < count; ++i) {
    co_yield i;
  }
  throw std::runtime_error("generator error");
}

TEST_CASE("Generator produces infinite sequence", "[generator]") {
  std::vector<int> got;
  auto fn = [&]() -> Task<> {
    auto gen = fibonacci();
    for (int i = 0; i < 8; ++i) {
      auto val = co_await gen.next();
      REQUIRE(val.has_value());
      got.push_back(*val);
    }
  };
  test::run_task<void>(fn());
  CHECK(got == std::vector<int>{0, 1, 1, 2, 3, 5, 8, 13});
}

TEST_CASE("Generator finite sequence ends with nullopt", "[generator]") {
  std::vector<int> got;
  bool ended = false;
  auto fn = [&]() -> Task<> {
    auto gen = finite(3);
    for (;;) {
      auto val = co_await gen.next();
      if (!val) {
        ended = true;
        break;
      }
      got.push_back(*val);
    }
  };
  test::run_task<void>(fn());
  CHECK(got == std::vector<int>{0, 1, 2});
  CHECK(ended);
}

TEST_CASE("Generator zero-length ends immediately", "[generator]") {
  bool ended = false;
  auto fn = [&]() -> Task<> {
    auto gen = finite(0);
    auto val = co_await gen.next();
    ended = !val.has_value();
  };
  test::run_task<void>(fn());
  CHECK(ended);
}

TEST_CASE("Generator exception propagates to consumer", "[generator]") {
  auto fn = [&]() -> Task<> {
    auto gen = throws_after(2);
    co_await gen.next();
    co_await gen.next();
    co_await gen.next();
  };
  CHECK_THROWS_AS(test::run_task<void>(fn()), std::runtime_error);
}
