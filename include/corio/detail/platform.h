#pragma once

#ifdef __linux__
#include <corio/detail/linux/io_uring.h>
namespace corio::detail {
using TargetPoller = URingPoller;
} // namespace corio::detail
#else
#error "Unsupported platform. Implement Poller for this OS."
#endif
