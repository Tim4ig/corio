#pragma once

#include <type_traits>
#include <variant>

namespace corio::detail {
/// Maps void to monostate so every task result has a concrete type, usable
/// as a std::tuple element (gather) or std::variant alternative (race).
template <typename T> using GatherVal = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
} // namespace corio::detail
