#include "helpers.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <corio/detail/thread_pool.h>
#include <corio/io_context.h>
#include <corio/race.h>
#include <corio/task.h>
#include <corio/timer.h>
#include <corio/to_thread.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

using namespace corio;
using namespace std::chrono_literals;

namespace {
/// Runs every job synchronously on the calling (IoContext) thread. Lets
/// tests prove set_thread_pool() is actually consulted.
class InlinePool : public detail::ThreadPool {
 public:
  int calls = 0;

  void submit(std::function<void()> job) override {
    ++calls;
    job();
  }
};
} // namespace

TEST_CASE("to_thread runs the callable and returns its value", "[to_thread]") {
  auto result = test::run_task<int>([]() -> Task<int> {
    co_return co_await to_thread([] {
      return 21 * 2;
    });
  }());
  CHECK(result == 42);
}

TEST_CASE("to_thread propagates an exception from the callable", "[to_thread]") {
  auto run = [] {
    test::run_task<int>([]() -> Task<int> {
      co_return co_await to_thread([]() -> int {
        throw std::runtime_error("thread failure");
      });
    }());
  };
  CHECK_THROWS_AS(run(), std::runtime_error);
}

TEST_CASE("to_thread runs off the IoContext thread by default", "[to_thread]") {
  auto worker_id = std::this_thread::get_id();
  test::run_task<void>([&]() -> Task<> {
    worker_id = co_await to_thread([] {
      return std::this_thread::get_id();
    });
  }());
  CHECK(worker_id != std::this_thread::get_id());
}

TEST_CASE("to_thread with a void callable resumes with no value", "[to_thread]") {
  auto ran = false;
  test::run_task<void>([&]() -> Task<> {
    co_await to_thread([&] {
      ran = true;
    });
  }());
  CHECK(ran);
}

TEST_CASE("set_thread_pool installs a custom pool that to_thread uses", "[to_thread]") {
  auto ctx = make_io_context();
  auto pool = std::make_shared<InlinePool>();
  ctx.set_thread_pool(pool);

  auto result = 0;
  auto fn = [&]() -> Task<> {
    result = co_await to_thread([] {
      return 7;
    });
    ctx.stop();
  };
  const auto task = fn();
  ctx.post(task.native_handle());
  ctx.run();

  CHECK(result == 7);
  CHECK(pool->calls == 1);
}

TEST_CASE("racing to_thread against a timeout abandons the slow job's resume", "[to_thread]") {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();

  auto timeout = [](auto dur) -> Task<> {
    co_await async_sleep(dur);
  };
  auto slow = []() -> Task<int> {
    co_return co_await to_thread([] {
      std::this_thread::sleep_for(200ms);
      return 1;
    });
  };

  auto outcome = test::run_task<std::variant<int, std::monostate>>([&]() -> Task<std::variant<int, std::monostate>> {
    co_return co_await race(slow(), timeout(10ms));
  }());
  const auto elapsed = Clock::now() - start;

  REQUIRE(outcome.index() == 1);
  CHECK(elapsed < 100ms);
}
