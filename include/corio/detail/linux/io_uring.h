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
///
/// Every armed poll carries a generation counter in its user_data, so a
/// stale CQE (from an instance that was cancelled, replaced by mod(), or
/// armed for a since-recycled fd number) never masquerades as the current
/// registration.
///
/// SQEs are batched: add()/mod()/rem() only queue entries; the single
/// io_uring_submit at poll() entry flushes them (get_sqe flushes early when
/// the submission queue fills up).
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

  // user_data layout: bits 0-31 fd, bits 32-39 op, bits 40-63 generation.
  enum class Op : std::uint8_t { kPollAdd, kPollRemove, kWakeup };

  static constexpr std::uint64_t kGenMask = 0xFF'FFFF;
  static constexpr unsigned kOpShift = 32;
  static constexpr unsigned kGenShift = 40;

  static std::uint64_t encode(Op op, int fd, std::uint32_t gen) noexcept {
    return ((static_cast<std::uint64_t>(gen) & kGenMask) << kGenShift) |
           (static_cast<std::uint64_t>(std::to_underlying(op)) << kOpShift) | static_cast<std::uint32_t>(fd);
  }

  static Op decode_op(std::uint64_t data) noexcept {
    return static_cast<Op>((data >> kOpShift) & 0xFF);
  }

  static int decode_fd(std::uint64_t data) noexcept {
    return static_cast<int>(data & 0xFFFF'FFFF);
  }

  static std::uint32_t decode_gen(std::uint64_t data) noexcept {
    return static_cast<std::uint32_t>((data >> kGenShift) & kGenMask);
  }

  static std::uint32_t to_poll_mask(IOEvent events) noexcept;
  static IOEvent from_poll_mask(std::uint32_t mask) noexcept;

  // Returns a fresh SQE, submitting the current batch first if the SQ is full.
  io_uring_sqe* get_sqe();

  std::uint32_t submit_poll_add(int fd, IOEvent events);
  void submit_poll_remove(int fd, std::uint32_t gen);
  void rearm_wakeup();

  // Block until at least one CQE is ready. Returns false on timeout.
  [[nodiscard]] bool wait_for_first_cqe(std::chrono::milliseconds timeout);

  // Consume one CQE and append any resulting IOToken to result.
  void consume_cqe(const io_uring_cqe* cqe, std::vector<IOToken>& result, bool& woke);

  io_uring ring_{};
  bool ring_initialized_{false};
  int wakeup_fd_{-1};
  std::uint32_t next_gen_{0};

  struct FdInfo {
    IOEvent events{};
    void* user{nullptr};
    std::uint32_t gen{0};
  };
  // fds currently waiting for a POLL_ADD CQE (not yet fired)
  std::unordered_map<int, FdInfo> registered_;
};

} // namespace corio::detail
