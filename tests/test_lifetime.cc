#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <corio/cancellation.h>
#include <corio/io_context.h>
#include <corio/task.h>
#include <corio/timer.h>
#include <memory>
#include <thread>

using namespace corio;
using namespace std::chrono_literals;

// Regression tests for the ready-queue use-after-free: destroying a task
// whose event already fired (its resume is queued but not yet dispatched)
// must scrub the queued handle instead of resuming a freed frame.

TEST_CASE("destroying a task with a queued cancellation resume never resumes it", "[lifetime]") {
  const CancellationSource src;
  auto ctx = make_io_context();
  auto victim_resumed = false;
  std::unique_ptr<Task<>> victim;

  auto victim_fn = [&]() -> Task<> {
    co_await src.token().wait();
    victim_resumed = true;
  };
  auto killer_fn = [&]() -> Task<> {
    co_await src.token().wait();
    // Both waiters were posted together by request_cancellation(); the
    // victim's resume is sitting in the ready queue right now.
    victim.reset();
    ctx.stop();
  };

  const auto killer = killer_fn();
  victim = std::make_unique<Task<>>(victim_fn());

  // The killer registers first, so it is resumed first.
  ctx.post(killer.native_handle());
  ctx.post(victim->native_handle());

  std::thread requester{[&] {
    std::this_thread::sleep_for(20ms);
    src.request_cancellation();
  }};
  ctx.run();
  requester.join();

  CHECK_FALSE(victim_resumed);
}

TEST_CASE("destroying a sleeping task after its deadline fired is safe", "[lifetime]") {
  auto ctx = make_io_context();
  std::unique_ptr<Task<>> victim;

  auto victim_fn = []() -> Task<> {
    co_await async_sleep(10ms);
  };
  auto killer_fn = [&]() -> Task<> {
    co_await async_sleep(10ms); // same deadline: both expire in one batch
    victim.reset();
    ctx.stop();
  };

  const auto killer = killer_fn();
  victim = std::make_unique<Task<>>(victim_fn());

  ctx.post(killer.native_handle());
  ctx.post(victim->native_handle());
  ctx.run();

  SUCCEED("no use-after-free under ASan");
}

TEST_CASE("destroying a sleeping task before its deadline cancels the timer", "[lifetime]") {
  auto ctx = make_io_context();
  auto victim_resumed = false;
  std::unique_ptr<Task<>> victim;

  auto victim_fn = [&]() -> Task<> {
    co_await async_sleep(10s); // far in the future
    victim_resumed = true;
  };
  auto killer_fn = [&]() -> Task<> {
    co_await async_sleep(10ms);
    victim.reset(); // drops the pending timer via the awaitable destructor
    ctx.stop();
  };

  const auto killer = killer_fn();
  victim = std::make_unique<Task<>>(victim_fn());

  ctx.post(killer.native_handle());
  ctx.post(victim->native_handle());

  const auto start = std::chrono::steady_clock::now();
  ctx.run();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK_FALSE(victim_resumed);
  CHECK(elapsed < 5s); // loop exited without waiting for the dead deadline
}
