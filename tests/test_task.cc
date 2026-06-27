#include "helpers.h"

#include <corio/task.h>

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace corio;

static Task<int> return_value(int val) {
    co_return val;
}

static Task<> throw_logic_error() {
    throw std::logic_error("boom");
    co_return;
}

static Task<int> chain(int val) {
    int result = co_await return_value(val + 1);
    co_return result * 2;
}

static Task<> deep_chain(int depth, int& counter) {
    if (depth == 0) {
        ++counter;
        co_return;
    }
    co_await deep_chain(depth - 1, counter);
}

TEST_CASE("Task<void> completes", "[task]") {
    bool ran = false;
    auto fn = [&]() -> Task<> { ran = true; co_return; };
    test::run_task<void>(fn());
    CHECK(ran);
}

TEST_CASE("Task<int> returns value", "[task]") {
    int val = test::run_task<int>(return_value(42));
    CHECK(val == 42);
}

TEST_CASE("Task chain propagates value", "[task]") {
    int val = test::run_task<int>(chain(5));
    CHECK(val == 12);
}

TEST_CASE("Task exception propagates to caller", "[task]") {
    CHECK_THROWS_AS(test::run_task<void>(throw_logic_error()), std::logic_error);
}

TEST_CASE("Task is lazy (frame not entered until resumed)", "[task]") {
    bool entered = false;
    auto task_fn = [&]() -> Task<> { entered = true; co_return; };
    auto task = task_fn();
    CHECK_FALSE(entered);
    test::run_task<void>(std::move(task));
    CHECK(entered);
}

TEST_CASE("Deep task chain does not stack overflow (symmetric transfer)", "[task]") {
    int counter = 0;
    auto fn = [&]() -> Task<> { co_await deep_chain(10'000, counter); };
    test::run_task<void>(fn());
    CHECK(counter == 1);
}

TEST_CASE("Task move semantics: moved-from task is empty", "[task]") {
    auto a = return_value(1);
    auto b = std::move(a);
    CHECK(a.done());
    CHECK_FALSE(b.done());
}
