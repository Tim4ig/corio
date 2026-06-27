#include "helpers.h"

#include <corio/gather.h>
#include <corio/task.h>
#include <corio/timer.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>

using namespace corio;
using namespace std::chrono_literals;

TEST_CASE("async_sleep suspends for at least the requested duration", "[timer]") {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();

    auto fn = []() -> Task<> { co_await async_sleep(30ms); };
    test::run_task<void>(fn());

    auto elapsed = Clock::now() - start;
    CHECK(elapsed >= 25ms);
    CHECK(elapsed < 200ms);
}

TEST_CASE("async_sleep zero duration returns immediately", "[timer]") {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();

    auto fn = []() -> Task<> { co_await async_sleep(0ms); };
    test::run_task<void>(fn());

    auto elapsed = Clock::now() - start;
    CHECK(elapsed < 50ms);
}

TEST_CASE("sequential async_sleeps accumulate time", "[timer]") {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();

    auto fn = []() -> Task<> {
        co_await async_sleep(20ms);
        co_await async_sleep(20ms);
    };
    test::run_task<void>(fn());

    auto elapsed = Clock::now() - start;
    CHECK(elapsed >= 35ms);
    CHECK(elapsed < 200ms);
}

TEST_CASE("multiple independent tasks each sleep their own duration", "[timer]") {
    using Clock = std::chrono::steady_clock;
    std::chrono::milliseconds durations[3]{};

    auto fn = [&]() -> Task<> {
        auto t = [](std::chrono::milliseconds dur,
                    std::chrono::milliseconds& out) -> Task<> {
            auto s = Clock::now();
            co_await async_sleep(dur);
            out = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s);
            co_return;
        };
        co_await gather(t(10ms, durations[0]), t(20ms, durations[1]), t(30ms, durations[2]));
        co_return;
    };
    test::run_task<void>(fn());

    CHECK(durations[0] >= 8ms);
    CHECK(durations[1] >= 18ms);
    CHECK(durations[2] >= 28ms);
}
