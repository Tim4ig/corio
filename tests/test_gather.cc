#include "helpers.h"

#include <corio/gather.h>
#include <corio/task.h>
#include <corio/timer.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <tuple>

using namespace corio;
using namespace std::chrono_literals;

static Task<int> immediate(int val) { co_return val; }
static Task<>    immediate_void()   { co_return; }
static Task<int> throws_int()       { throw std::runtime_error("child error"); co_return 0; }

TEST_CASE("gather all void tasks completes", "[gather]") {
    int counter = 0;
    auto fn = [&]() -> Task<> {
        auto a_fn = [&]() -> Task<> { ++counter; co_return; };
        auto b_fn = [&]() -> Task<> { ++counter; co_return; };
        auto c_fn = [&]() -> Task<> { ++counter; co_return; };
        co_await gather(a_fn(), b_fn(), c_fn());
    };
    test::run_task<void>(fn());
    CHECK(counter == 3);
}

TEST_CASE("gather returns values in argument order", "[gather]") {
    auto [a, b, c] = test::run_task<std::tuple<int, int, int>>(
        gather(immediate(10), immediate(20), immediate(30))
    );
    CHECK(a == 10);
    CHECK(b == 20);
    CHECK(c == 30);
}

TEST_CASE("gather mixed void and value tasks", "[gather]") {
    auto [mono, val] = test::run_task<std::tuple<std::monostate, int>>(
        gather(immediate_void(), immediate(99))
    );
    CHECK(val == 99);
}

TEST_CASE("gather with zero tasks completes immediately", "[gather]") {
    bool ran = false;
    auto fn = [&]() -> Task<> {
        co_await gather();
        ran = true;
    };
    test::run_task<void>(fn());
    CHECK(ran);
}

TEST_CASE("gather exception from child propagates", "[gather]") {
    auto run = [] {
        using R = std::tuple<int, int>;
        test::run_task<R>(gather(immediate(1), throws_int()));
    };
    CHECK_THROWS_AS(run(), std::runtime_error);
}

TEST_CASE("gather tasks run concurrently (timing)", "[gather]") {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();

    auto sleep = [](auto dur) -> Task<> { co_await async_sleep(dur); co_return; };
    auto fn    = [&]() -> Task<> { co_await gather(sleep(50ms), sleep(50ms), sleep(50ms)); };
    test::run_task<void>(fn());

    auto elapsed = Clock::now() - start;
    CHECK(elapsed < 120ms);
    CHECK(elapsed >= 40ms);
}
