#include <corio/gather.h>
#include <corio/generator.h>
#include <corio/io_context.h>
#include <corio/task.h>
#include <corio/timer.h>
#include <iostream>

using namespace std::chrono_literals;

namespace {

// --- Generator demo ---------------------------------------------------------

corio::Generator<int> fibonacci() {
  int prev = 0;
  int curr = 1;
  for (;;) {
    co_yield prev;
    int next = prev + curr;
    prev = curr;
    curr = next;
  }
}

corio::Task<> gen_demo() {
  auto fib = fibonacci();
  std::cout << "fib: ";
  for (int i = 0; i < 10; ++i) {
    auto val = co_await fib.next();
    std::cout << *val << " ";
  }
  std::cout << "\n";
}

// --- Gather demo ------------------------------------------------------------

corio::Task<int> compute(int val, std::chrono::milliseconds delay) {
  co_await corio::async_sleep(delay);
  co_return val* val;
}

corio::Task<> gather_demo(corio::IoContext* ctx) {
  std::cout << "gather: launching 3 tasks (300ms, 100ms, 200ms)...\n";

  // All three run concurrently -- total time ~= max(300, 100, 200) = 300ms
  auto [aa, bb, cc] = co_await corio::gather(compute(2, 300ms), compute(3, 100ms), compute(5, 200ms));

  std::cout << "gather: 2^2=" << aa << " 3^2=" << bb << " 5^2=" << cc << "\n";
  ctx->stop();
}

corio::Task<> root(corio::IoContext* ctx) {
  co_await gen_demo();
  co_await gather_demo(ctx);
}

} // namespace

int main() {
  auto ctx = corio::make_io_context();
  auto task = root(&ctx);
  ctx.post(task.native_handle());
  ctx.run();
}
