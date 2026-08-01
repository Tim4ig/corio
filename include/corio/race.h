#pragma once

#include <array>
#include <corio/detail/void_result.h>
#include <corio/error.h>
#include <corio/io_context.h>
#include <corio/task.h>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace corio {
namespace detail {
/// Shared state in the race coroutine's frame.
/// race_child holds a reference into this struct -- safe because race is
/// suspended for the entire lifetime of its children.
template <typename Variant> struct RaceState {
  std::coroutine_handle<> parent;
  std::optional<Variant> result;
  std::exception_ptr exception;
  bool decided = false;
};

/// Wrapper coroutine: drives one task. The first child to finish (by value
/// or exception) decides the race and posts the parent; every later
/// finisher sees decided == true and returns without touching state again.
template <typename T, typename Variant, std::size_t Index> Task<> race_child(Task<T> task, RaceState<Variant>& state) {
  auto won = false;
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(task);
      if (!state.decided) {
        state.decided = true;
        won = true;
        state.result.emplace(std::in_place_index<Index>, std::monostate{});
      }
    } else {
      auto val = co_await std::move(task);
      if (!state.decided) {
        state.decided = true;
        won = true;
        state.result.emplace(std::in_place_index<Index>, std::move(val));
      }
    }
  } catch (...) {
    if (!state.decided) {
      state.decided = true;
      won = true;
      state.exception = std::current_exception();
    }
  }
  if (won) {
    IoContext::current()->post(state.parent);
  }
}

/// Suspends the caller until state.decided becomes true.
template <typename Variant> struct RaceAwaitable {
  RaceState<Variant>* state;

  [[nodiscard]] bool await_ready() const noexcept {
    return state->decided;
  }

  void await_suspend(std::coroutine_handle<> handle) const noexcept {
    state->parent = handle;
  }

  static void await_resume() noexcept {
  }
};

} // namespace detail

/// @brief Run all tasks concurrently; resume as soon as the first one
///        finishes (by value or exception) and abandon the rest.
///
/// Every task not yet finished when the race is decided has its wrapper
/// coroutine destroyed while suspended. This is safe exactly to the extent
/// that corio's cancel-on-destroy contract holds for the awaitables in play:
/// async_sleep, CancellationToken::wait(), and any I/O awaitable following
/// the pattern documented in the README (register interest in
/// await_suspend, deregister in the destructor). A racing task built from
/// well-behaved awaitables leaves no dangling IoContext registration when
/// it loses.
///
/// @tparam Ts  Result types of each task. void tasks produce std::monostate.
/// @return Task<std::variant<detail::GatherVal<Ts>...>> holding the winner's
///         result at the alternative matching its argument position.
///
/// @note Must be called from within IoContext::run().
/// @note Requires at least one task.
///
/// @code
/// // Timeout composition: race an operation against a deadline.
/// auto timeout = [](auto dur) -> Task<> { co_await corio::async_sleep(dur); };
/// auto outcome = co_await corio::race(do_io(), timeout(5s));
/// if (outcome.index() == 1) {
///   // timed out; do_io()'s task was abandoned and its I/O interest
///   // deregistered by its own awaitable destructors.
/// }
/// @endcode
template <typename... Ts> Task<std::variant<detail::GatherVal<Ts>...>> race(Task<Ts>... tasks) {
  static_assert(sizeof...(Ts) > 0, "corio::race requires at least one task");

  using Variant = std::variant<detail::GatherVal<Ts>...>;
  auto state = detail::RaceState<Variant>{};

  auto task_tuple = std::make_tuple(std::move(tasks)...);

  auto children = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    return std::array<Task<>, sizeof...(Ts)>{
        detail::race_child<Ts, Variant, Is>(std::move(std::get<Is>(task_tuple)), state)...};
  }(std::make_index_sequence<sizeof...(Ts)>{});

  auto* ctx = IoContext::current();
  if (ctx == nullptr) {
    throw corio::NoContextError("corio::race requires a running IoContext");
  }
  for (auto& child : children) {
    ctx->post(child.native_handle());
  }

  co_await detail::RaceAwaitable<Variant>{&state};

  // Every child still holding a suspended frame at this point is abandoned
  // here: `children` goes out of scope and each Task<>'s destructor
  // destroys its coroutine, cascading into whatever awaitable it was
  // suspended on. The winner is already done(), so its destruction is a
  // plain, uneventful frame teardown.

  if (state.exception) {
    std::rethrow_exception(state.exception);
  }
  co_return std::move(*state.result);
}
} // namespace corio
