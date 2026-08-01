#pragma once

#include <stdexcept>
#include <system_error>

namespace corio {

/// Abstract base for all corio exceptions. Catch this to handle any error
/// originating from the corio runtime without caring about the specific kind.
///
/// Deliberately does NOT inherit std::exception. LogicError/SystemError/
/// InternalError each multiply-inherit Error plus the matching std
/// exception type (std::logic_error/system_error/runtime_error), and the
/// std types already inherit std::exception non-virtually. Giving Error an
/// std::exception base too would force virtual inheritance on Error's side
/// while the std types stay non-virtual, producing two distinct
/// std::exception subobjects and an ambiguous conversion to
/// `const std::exception&` -- an unfixable diamond, since the standard
/// mandates std::logic_error's inheritance be non-virtual. Keeping Error a
/// pure interface sidesteps that; every leaf still ends up std::exception-
/// derived through its std-exception base, so `catch (const std::exception&)`
/// keeps working.
///
/// Contract for new exception types: derive from LogicError, SystemError,
/// or InternalError (or add a sibling category following the same
/// Error + std-exception pattern) -- never from Error directly. A type that
/// only inherits Error would compile (what() is satisfied) but would not be
/// std::exception-derived, silently escaping generic `catch (const
/// std::exception&)` handlers elsewhere in a consuming application.
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
/// Also, catchable as std::logic_error.
class LogicError : public Error, public std::logic_error {
 public:
  using std::logic_error::logic_error;
  [[nodiscard]] const char* what() const noexcept override {
    return std::logic_error::what();
  }
};

/// Base for OS-level failures. Also, catchable as std::system_error.
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
