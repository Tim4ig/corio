#pragma once

#include <chrono>
#include <corio/detail/platform.h>

namespace corio {
/// @brief Suspend the current coroutine for at least dur.
///
/// The coroutine is resumed by the owning IoContext once the duration
/// elapses. No OS thread is blocked while waiting.
///
/// @note Must be called from within IoContext::run().
///
/// @code
/// co_await corio::async_sleep(std::chrono::milliseconds{100});
/// @endcode
template <typename Rep, typename Period>
[[nodiscard]] detail::SleepAwaitable async_sleep(std::chrono::duration<Rep, Period> dur) {
  return detail::SleepAwaitable{std::chrono::duration_cast<std::chrono::nanoseconds>(dur)};
}
} // namespace corio
