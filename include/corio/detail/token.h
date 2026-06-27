#pragma once

#include <corio/detail/types.h>
#include <utility>

namespace corio {
/// Abstract I/O readiness events. Values are library-internal; the platform
/// layer translates them to epoll/kqueue constants.
enum struct IOEvent : u8 {
  kNone = 0,
  kR = 1 << 0,
  kW = 1 << 1,
  kErr = 1 << 2,
};

[[nodiscard]] constexpr IOEvent operator|(IOEvent lhs, IOEvent rhs) noexcept {
  return static_cast<IOEvent>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

[[nodiscard]] constexpr IOEvent operator&(IOEvent lhs, IOEvent rhs) noexcept {
  return static_cast<IOEvent>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

[[nodiscard]] constexpr bool any(IOEvent ev) noexcept {
  return std::to_underlying(ev) != 0;
}

/// Fired token returned by Poller::poll(). The user pointer carries whatever
/// the caller stored in Poller::add()/mod().
struct IOToken {
  int fd;
  IOEvent events;
  void* user;
};
} // namespace corio
