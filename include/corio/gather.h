#pragma once

#include <array>
#include <corio/io_context.h>
#include <corio/task.h>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

namespace corio {
namespace detail {
/// Maps void to monostate so every element of the result tuple is concrete.
template <typename T> using GatherVal = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

/// Shared state in the gather coroutine's frame.
/// gather_child holds references into this struct -- safe because gather is
/// suspended for the entire lifetime of its children.
struct GatherState {
  std::size_t remaining;
  std::coroutine_handle<> parent;
  std::exception_ptr first_exception;
};

/// Wrapper coroutine: drives one task, stores its result, then decrements
/// the shared counter. When the counter reaches zero, posts the parent.
template <typename T> Task<> gather_child(Task<T> task, GatherVal<T>& out, GatherState& state) {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(task);
    } else {
      out = co_await std::move(task);
    }
  } catch (...) {
    if (!state.first_exception) {
      state.first_exception = std::current_exception();
    }
  }
  if (--state.remaining == 0) {
    IoContext::current()->post(state.parent);
  }
}

/// Suspends the caller until state.remaining drops to zero.
struct GatherAwaitable {
  GatherState* state;

  [[nodiscard]] bool await_ready() const noexcept {
    return state->remaining == 0;
  }

  void await_suspend(std::coroutine_handle<> handle) const noexcept {
    state->parent = handle;
  }

  static void await_resume() noexcept {
  }
};

} // namespace detail

/// @brief Run all tasks concurrently on the current IoContext.
///
/// Schedules every task and suspends the caller until all have finished.
/// Results are returned as a tuple in argument order.
///
/// @tparam Ts  Result types of each task.
///             void tasks produce std::monostate in the tuple.
///
/// @return Task<tuple<GatherVal<Ts>...>>
///
/// @note Must be called from within IoContext::run().
/// @note If multiple tasks throw, only the first exception is rethrown
///       (after all tasks have completed).
///
/// @code
/// auto [a, b] = co_await corio::gather(
///     compute_a(),
///     compute_b()
/// );
/// @endcode
template <typename... Ts> Task<std::tuple<detail::GatherVal<Ts>...>> gather(Task<Ts>... tasks) {
  auto state = detail::GatherState{.remaining = sizeof...(Ts), .parent = {}, .first_exception = {}};
  std::tuple<detail::GatherVal<Ts>...> results;

  auto task_tuple = std::make_tuple(std::move(tasks)...);

  auto children = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    return std::array<Task<>, sizeof...(Ts)>{
        detail::gather_child(std::move(std::get<Is>(task_tuple)), std::get<Is>(results), state)...};
  }(std::make_index_sequence<sizeof...(Ts)>{});

  auto* ctx = IoContext::current();
  for (auto& child : children) {
    ctx->post(child.native_handle());
  }

  co_await detail::GatherAwaitable{&state};

  if (state.first_exception) {
    std::rethrow_exception(state.first_exception);
  }
  co_return std::move(results);
}
} // namespace corio
