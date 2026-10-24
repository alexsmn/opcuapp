#include "opcua/transport/binary/egress_meter.h"

#include <algorithm>
#include <utility>

namespace opcua::binary {

EgressBudget::EgressBudget(std::uint64_t bytes_per_day,
                           std::function<DateTime()> now)
    : bytes_per_day_{bytes_per_day}, now_{std::move(now)} {}

void EgressBudget::RollOver() {
  const std::int64_t day = (now_() - DateTime::UnixEpoch()).InDays();
  if (day != day_) {
    day_ = day;
    sent_today_ = 0;
  }
}

bool EgressBudget::exhausted() {
  if (bytes_per_day_ == 0)
    return false;
  RollOver();
  return sent_today_ >= bytes_per_day_;
}

void EgressBudget::Record(std::size_t bytes) {
  RollOver();
  sent_today_ += bytes;
  sent_total_ += bytes;
}

std::uint64_t EgressBudget::sent_today() {
  RollOver();
  return sent_today_;
}

ConnectionPacer::ConnectionPacer(std::size_t bytes_per_second,
                                 std::function<DateTime()> now)
    : bytes_per_second_{bytes_per_second},
      now_{std::move(now)},
      tokens_{static_cast<double>(bytes_per_second)},
      last_{now_()} {}

Duration ConnectionPacer::DelayFor(std::size_t bytes) {
  if (bytes_per_second_ == 0)
    return Duration{};
  const DateTime now = now_();
  const double rate = static_cast<double>(bytes_per_second_);
  // Refill for the time since the last write, capped at a one-second burst.
  tokens_ = std::min(rate, tokens_ + (now - last_).InSecondsF() * rate);
  last_ = now;
  tokens_ -= static_cast<double>(bytes);
  if (tokens_ >= 0)
    return Duration{};
  // The write may go once the debt has refilled.
  return Duration::FromMicroseconds(
      static_cast<std::int64_t>(-tokens_ / rate * 1e6));
}

}  // namespace opcua::binary
