#include "helpers.h"

#include <corio/cancellation.h>
#include <corio/task.h>
#include <corio/timer.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace corio;
using namespace std::chrono_literals;

TEST_CASE("CancellationToken not cancelled by default", "[cancellation]") {
    CancellationSource src;
    CHECK_FALSE(src.is_cancellation_requested());
    CHECK_FALSE(src.token().is_cancellation_requested());
}

TEST_CASE("request_cancellation sets the flag", "[cancellation]") {
    CancellationSource src;
    src.request_cancellation();
    CHECK(src.is_cancellation_requested());
    CHECK(src.token().is_cancellation_requested());
}

TEST_CASE("request_cancellation is idempotent", "[cancellation]") {
    CancellationSource src;
    src.request_cancellation();
    src.request_cancellation();
    CHECK(src.is_cancellation_requested());
}

TEST_CASE("co_await token.wait() suspends until request_cancellation()", "[cancellation]") {
    CancellationSource src;
    auto token = src.token();
    bool after_wait = false;

    test::run([&](IoContext* ctx) -> Task<> {
        auto canceller_fn = [&]() -> Task<> {
            co_await async_sleep(20ms);
            src.request_cancellation();
        };
        auto canceller = canceller_fn();
        ctx->post(canceller.native_handle());

        co_await token.wait();
        after_wait = true;
        ctx->stop();
    });

    CHECK(after_wait);
}

TEST_CASE("co_await already-cancelled token returns immediately", "[cancellation]") {
    CancellationSource src;
    src.request_cancellation();
    bool ran = false;

    test::run([&](IoContext* ctx) -> Task<> {
        co_await src.token().wait();
        ran = true;
        ctx->stop();
    });

    CHECK(ran);
}

TEST_CASE("request_cancellation from another thread wakes all waiters", "[cancellation]") {
    CancellationSource src2;
    int woken2 = 0;

    auto ctx2 = make_io_context();

    // Tasks owned at test scope so they are never destroyed while queued in the event loop.
    auto waiter_fn  = [&]() -> Task<> { co_await src2.token().wait(); ++woken2; };
    auto stopper_fn = [&]() -> Task<> { co_await async_sleep(200ms); ctx2.stop(); };
    auto w1 = waiter_fn();
    auto w2 = waiter_fn();
    auto stopper = stopper_fn();

    ctx2.post(w1.native_handle());
    ctx2.post(w2.native_handle());
    ctx2.post(stopper.native_handle());

    std::thread th{[&] {
        std::this_thread::sleep_for(20ms);
        src2.request_cancellation();
    }};

    ctx2.run();
    th.join();

    CHECK(woken2 == 2);
}

TEST_CASE("CancellationSource outlives CancellationToken", "[cancellation]") {
    CancellationToken token = []{
        CancellationSource src;
        return src.token();
    }();
    CHECK_FALSE(token.is_cancellation_requested());
}
