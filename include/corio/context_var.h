#pragma once

#include <any>
#include <corio/detail/context.h>
#include <corio/io_context.h>
#include <optional>

namespace corio {
/// @brief Task-local variable. Each task gets its own copy of the context,
///        isolated from sibling tasks but inherited from the spawner at post().
///
/// ContextVar instances are global or static; they act as typed keys into the
/// per-task context map.
///
/// @code
/// inline corio::ContextVar<RequestCtx> request_ctx;
///
/// corio::Task<> handle_connection(int fd) {
///     request_ctx.set(RequestCtx{.id = next_id()});
///     co_await middleware(fd);   // sees the same RequestCtx
/// }
/// @endcode
template <typename T> class ContextVar {
 public:
  /// @brief Set the value for the current task.
  void set(T value) const {
    auto& ctx = IoContext::current_context();
    if (!ctx) {
      ctx = std::make_shared<detail::ContextMap>();
    }
    (*ctx)[key()] = std::move(value);
  }

  /// @brief Get the value for the current task, or nullopt if not set.
  [[nodiscard]] std::optional<T> get() const {
    const auto& ctx = IoContext::current_context();
    if (!ctx) {
      return std::nullopt;
    }
    const auto it = ctx->find(key());
    if (it == ctx->end()) {
      return std::nullopt;
    }
    return std::any_cast<T>(it->second);
  }

  /// @brief Get the value or a provided default.
  [[nodiscard]] T get_or(T default_val) const {
    return get().value_or(std::move(default_val));
  }

 private:
  [[nodiscard]] const void* key() const noexcept {
    return this;
  }
};
} // namespace corio
