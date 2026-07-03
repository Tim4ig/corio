#include "helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <corio/gather.h>
#include <corio/io_context.h>
#include <corio/task.h>
#include <coroutine>
#include <cstddef>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace corio;

namespace {
/// Canonical readiness awaitable for the IO layer: syscall first, wait on
/// EAGAIN, cancel interest (and any queued resume) when destroyed early.
struct WaitReadable {
  IoContext* ctx;
  int fd;
  std::coroutine_handle<> handle;
  bool registered = false;

  [[nodiscard]] static bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> coroutine) {
    handle = coroutine;
    ctx->watch_read(fd, coroutine);
    registered = true;
  }

  void await_resume() noexcept {
    registered = false;
  }

  ~WaitReadable() {
    if (registered && !ctx->cancel_read(fd, handle)) {
      (void)ctx->cancel_posted(handle);
    }
  }

  WaitReadable(IoContext* ctx_in, int fd_in)
    : ctx(ctx_in)
    , fd(fd_in) {
  }
  WaitReadable(const WaitReadable&) = delete;
  WaitReadable& operator=(const WaitReadable&) = delete;
  WaitReadable(WaitReadable&&) = delete;
  WaitReadable& operator=(WaitReadable&&) = delete;
};

struct WaitWritable {
  IoContext* ctx;
  int fd;
  std::coroutine_handle<> handle;
  bool registered = false;

  [[nodiscard]] static bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> coroutine) {
    handle = coroutine;
    ctx->watch_write(fd, coroutine);
    registered = true;
  }

  void await_resume() noexcept {
    registered = false;
  }

  ~WaitWritable() {
    if (registered && !ctx->cancel_write(fd, handle)) {
      (void)ctx->cancel_posted(handle);
    }
  }

  WaitWritable(IoContext* ctx_in, int fd_in)
    : ctx(ctx_in)
    , fd(fd_in) {
  }
  WaitWritable(const WaitWritable&) = delete;
  WaitWritable& operator=(const WaitWritable&) = delete;
  WaitWritable(WaitWritable&&) = delete;
  WaitWritable& operator=(WaitWritable&&) = delete;
};

constexpr std::size_t kPumpBytes = std::size_t{4} * 1024 * 1024;
constexpr std::size_t kIoBufBytes = std::size_t{64} * 1024;

void make_nonblocking_pair(int (&pair)[2]) {
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  for (const int fd : pair) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) fcntl is the POSIX way
    REQUIRE(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    // Small buffers force many EAGAIN / readiness round-trips.
    int size = 32 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
  }
}

Task<std::size_t> pump_out(IoContext* ctx, int fd, std::size_t total) {
  std::vector<char> buf(kIoBufBytes, 'x');
  std::size_t sent = 0;
  while (sent < total) {
    const auto chunk = std::min(buf.size(), total - sent);
    const auto nsent = send(fd, buf.data(), chunk, 0);
    if (nsent > 0) {
      sent += static_cast<std::size_t>(nsent);
      continue;
    }
    if (nsent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await WaitWritable{ctx, fd};
      continue;
    }
    break;
  }
  shutdown(fd, SHUT_WR);
  co_return sent;
}

Task<std::size_t> pump_in(IoContext* ctx, int fd) {
  std::vector<char> buf(kIoBufBytes);
  std::size_t received = 0;
  for (;;) {
    const auto nread = recv(fd, buf.data(), buf.size(), 0);
    if (nread > 0) {
      received += static_cast<std::size_t>(nread);
      continue;
    }
    if (nread == 0) {
      break; // EOF
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await WaitReadable{ctx, fd};
      continue;
    }
    break;
  }
  co_return received;
}

/// Reads from fd and writes everything back: exercises simultaneous
/// reader+writer interest on one fd (the FdState/mod() path).
Task<> echo(IoContext* ctx, int fd) {
  std::vector<char> buf(kIoBufBytes / 4);
  for (;;) {
    const auto nread = recv(fd, buf.data(), buf.size(), 0);
    if (nread == 0) {
      break;
    }
    if (nread < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await WaitReadable{ctx, fd};
        continue;
      }
      break;
    }
    const auto len = static_cast<std::size_t>(nread);
    std::size_t off = 0;
    while (off < len) {
      const auto written = send(fd, &buf[off], len - off, 0);
      if (written > 0) {
        off += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        co_await WaitWritable{ctx, fd};
        continue;
      }
      co_return;
    }
  }
  shutdown(fd, SHUT_WR);
}
} // namespace

TEST_CASE("readiness pump moves bytes writer -> reader", "[socket]") {
  int pair[2] = {-1, -1};
  make_nonblocking_pair(pair);

  std::size_t sent = 0;
  std::size_t received = 0;
  test::run([&](IoContext* ctx) -> Task<> {
    std::tie(sent, received) = co_await gather(pump_out(ctx, pair[0], kPumpBytes), pump_in(ctx, pair[1]));
    ctx->stop();
  });

  CHECK(sent == kPumpBytes);
  CHECK(received == kPumpBytes);
  close(pair[0]);
  close(pair[1]);
}

TEST_CASE("full-duplex echo drives reader and writer on one fd", "[socket]") {
  int pair[2] = {-1, -1};
  make_nonblocking_pair(pair);

  std::size_t sent = 0;
  std::size_t echoed = 0;
  test::run([&](IoContext* ctx) -> Task<> {
    auto [out, unused, back] =
        co_await gather(pump_out(ctx, pair[0], kPumpBytes), echo(ctx, pair[1]), pump_in(ctx, pair[0]));
    (void)unused;
    sent = out;
    echoed = back;
    ctx->stop();
  });

  CHECK(sent == kPumpBytes);
  CHECK(echoed == kPumpBytes);
  close(pair[0]);
  close(pair[1]);
}

TEST_CASE("reader sees EOF when the peer closes", "[socket]") {
  int pair[2] = {-1, -1};
  make_nonblocking_pair(pair);

  std::size_t received = kPumpBytes;
  test::run([&](IoContext* ctx) -> Task<> {
    shutdown(pair[0], SHUT_WR);
    received = co_await pump_in(ctx, pair[1]);
    ctx->stop();
  });

  CHECK(received == 0);
  close(pair[0]);
  close(pair[1]);
}

TEST_CASE("waiter on a closed fd wakes with an error instead of hanging", "[socket]") {
  // The context (ring + eventfd) is created first so that closing pair[0]
  // leaves its number genuinely unused -- otherwise the ring would recycle
  // it and the poll would silently watch the wrong file.
  auto ctx = make_io_context();
  int pair[2] = {-1, -1};
  make_nonblocking_pair(pair);
  close(pair[0]);

  auto woke = false;
  auto recv_errno = 0;
  auto fn = [&]() -> Task<> {
    co_await WaitReadable{&ctx, pair[0]};
    woke = true;
    char byte = 0;
    if (recv(pair[0], &byte, 1, 0) < 0) {
      recv_errno = errno;
    }
    ctx.stop();
  };
  const auto task = fn();
  ctx.post(task.native_handle());
  ctx.run();

  CHECK(woke);
  CHECK(recv_errno == EBADF);
  close(pair[1]);
}
