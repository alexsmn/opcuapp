#pragma once

#include "opcua/types/date_time.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace opcua::binary {

// A daily cap on the bytes one listener may send, shared by all of its
// connections. Exists for deployments whose binding constraint is a monthly
// egress allowance rather than CPU or memory: once the UTC day's budget is
// spent, the listener stops serving until the next UTC day, instead of the
// operator finding out from the bill.
class EgressBudget {
 public:
  // `bytes_per_day` of zero never runs out.
  EgressBudget(std::uint64_t bytes_per_day, std::function<DateTime()> now);

  // Whether today's budget is spent. Rolls over at 00:00 UTC.
  [[nodiscard]] bool exhausted();

  // Accounts `bytes` already sent.
  void Record(std::size_t bytes);

  // Bytes sent since 00:00 UTC, and since construction.
  [[nodiscard]] std::uint64_t sent_today();
  [[nodiscard]] std::uint64_t sent_total() const { return sent_total_; }

 private:
  void RollOver();

  std::uint64_t bytes_per_day_;
  std::function<DateTime()> now_;
  std::int64_t day_ = -1;
  std::uint64_t sent_today_ = 0;
  std::uint64_t sent_total_ = 0;
};

// A token bucket pacing one connection's writes to `bytes_per_second`, with a
// one-second burst. The caller asks how long to wait before a write of a given
// size and then waits that long; the bucket has already accounted the write.
class ConnectionPacer {
 public:
  // `bytes_per_second` of zero never delays.
  ConnectionPacer(std::size_t bytes_per_second, std::function<DateTime()> now);

  [[nodiscard]] Duration DelayFor(std::size_t bytes);

 private:
  std::size_t bytes_per_second_;
  std::function<DateTime()> now_;
  // Bytes the bucket may still send without waiting; negative is debt.
  double tokens_;
  DateTime last_;
};

}  // namespace opcua::binary
