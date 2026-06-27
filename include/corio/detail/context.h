#pragma once

#include <any>
#include <memory>
#include <unordered_map>

namespace corio::detail {

/// Per-task key-value store. Keys are addresses of ContextVar<T> instances.
using ContextMap = std::unordered_map<const void*, std::any>;

} // namespace corio::detail
