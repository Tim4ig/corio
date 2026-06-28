#pragma once

#ifdef __linux__
#include <corio/detail/linux/io_uring.h>
#include <corio/detail/linux/timer.h>
namespace corio::detail {
using TargetPoller = URingPoller;
using SleepAwaitable = TimerfdSleepAwaitable;
} // namespace corio::detail
#else
#error "Unsupported platform. Implement Poller and SleepAwaitable for this OS."
#endif
