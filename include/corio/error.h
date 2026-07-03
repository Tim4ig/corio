#pragma once

#include <stdexcept>
#include <system_error>

namespace corio {

/// Abstract base for all corio exceptions. Catch this to handle any error
/// originating from the corio runtime without caring about the specific kind.
class Error {
 public:
  virtual ~Error() = default;
  [[nodiscard]] virtual const char* what() const noexcept = 0;

 protected:
  Error() = default;
  Error(const Error&) = default;
  Error& operator=(const Error&) = default;
  Error(Error&&) = default;
  Error& operator=(Error&&) = default;
};

/// Base for API misuse errors: violated preconditions, wrong thread, etc.
/// Also catchable as std::logic_error.
class LogicError : public Error, public std::logic_error {
 public:
  using std::logic_error::logic_error;
  [[nodiscard]] const char* what() const noexcept override {
    return std::logic_error::what();
  }
};

/// Base for OS-level failures. Also catchable as std::system_error.
class SystemError : public Error, public std::system_error {
 public:
  using std::system_error::system_error;
  [[nodiscard]] const char* what() const noexcept override {
    return std::system_error::what();
  }
};

/// Base for internal resource exhaustion. Also catchable as std::runtime_error.
class InternalError : public Error, public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
  [[nodiscard]] const char* what() const noexcept override {
    return std::runtime_error::what();
  }
};

/// Attempted to use a null or moved-from Task, Generator, or CancellationToken.
class EmptyHandleError : public LogicError {
 public:
  using LogicError::LogicError;
};

/// Operation requires a running IoContext but none is active on this thread.
class NoContextError : public LogicError {
 public:
  using LogicError::LogicError;
};

/// A file descriptor already has a pending reader or writer registered.
class FdConflictError : public LogicError {
 public:
  using LogicError::LogicError;
};

/// IoContext method called from a thread other than its owning thread.
class ThreadViolationError : public LogicError {
 public:
  using LogicError::LogicError;
};

/// An io_uring syscall failed.
class IoUringError : public SystemError {
 public:
  using SystemError::SystemError;
};

} // namespace corio
