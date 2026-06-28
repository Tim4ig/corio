#pragma once

#include <corio/detail/poller.h>

#include <liburing.h>

#include <unordered_map>

namespace corio::detail {

/// io_uring(7) + eventfd(2) based Poller for Linux.
///
/// Each registered fd gets a one-shot IORING_OP_POLL_ADD SQE.
/// When the poll fires we return an IOToken; the fd is then un-registered
/// from the ring automatically (one-shot semantics).
///
/// mod() handles the post-fire case: if the fd is no longer in the ring
/// (already fired) it re-submits POLL_ADD; if it's still pending it cancels
/// the old SQE first then re-submits.
class URingPoller : public Poller {
 public:
  URingPoller();
  ~URingPoller() noexcept override;

  URingPoller(const URingPoller&) = delete;
  URingPoller& operator=(const URingPoller&) = delete;

  URingPoller(URingPoller&&) = delete;
  URingPoller& operator=(URingPoller&&) = delete;

  void add(int fd, IOEvent events, void* user) override;
  void mod(int fd, IOEvent events, void* user) override;
  void rem(int fd) override;
  void wake() noexcept override;
  std::vector<IOToken> poll(std::chrono::milliseconds timeout) override;

 private:
  static constexpr unsigned kQueueDepth = 256;

  // Encoded into io_uring SQE user_data so we can distinguish CQE types.
  enum class Op : std::uint8_t { kPollAdd, kPollRemove, kWakeup };

  static std::uint64_t encode(Op op, int fd) noexcept {
    return (static_cast<std::uint64_t>(std::to_underlying(op)) << 32) |
           static_cast<std::uint32_t>(fd);
  }

  static Op decode_op(std::uint64_t data) noexcept {
    return static_cast<Op>(data >> 32);
  }

  static int decode_fd(std::uint64_t data) noexcept {
    return static_cast<int>(data & 0xFFFF'FFFF);
  }

  static std::uint32_t to_poll_mask(IOEvent events) noexcept;
  static IOEvent from_poll_mask(std::uint32_t mask) noexcept;

  // Returns a fresh SQE, submitting the current batch first if the SQ is full.
  io_uring_sqe* get_sqe();

  void submit_poll_add(int fd, IOEvent events, void* user);
  void submit_poll_remove(int fd);
  void rearm_wakeup();

  // Block until at least one CQE is ready. Returns false on timeout.
  [[nodiscard]] bool wait_for_first_cqe(std::chrono::milliseconds timeout);

  // Consume one CQE and append any resulting IOToken to result.
  void consume_cqe(const io_uring_cqe* cqe, std::vector<IOToken>& result, bool& woke);

  io_uring ring_{};
  bool ring_initialized_{false};
  int wakeup_fd_{-1};

  struct FdInfo {
    IOEvent events;
    void* user{nullptr};
  };
  // fds currently waiting for a POLL_ADD CQE (not yet fired)
  std::unordered_map<int, FdInfo> registered_;
};

} // namespace corio::detail
