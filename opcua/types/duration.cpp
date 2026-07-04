#include "opcua/types/duration.h"

#include <cmath>
#include <limits>
#include <ostream>

namespace opcua {
namespace {

template <class Integer>
Integer SaturatingTrunc(double value) {
  if (std::isnan(value))
    return 0;
  if (value >= static_cast<double>(std::numeric_limits<Integer>::max()))
    return std::numeric_limits<Integer>::max();
  if (value <= static_cast<double>(std::numeric_limits<Integer>::min()))
    return std::numeric_limits<Integer>::min();
  return static_cast<Integer>(value);
}

}  // namespace

int Duration::InDays() const {
  return SaturatingTrunc<int>(milliseconds_ / kMillisecondsPerDay);
}

int Duration::InDaysFloored() const {
  return SaturatingTrunc<int>(std::floor(milliseconds_ / kMillisecondsPerDay));
}

int Duration::InHours() const {
  return SaturatingTrunc<int>(milliseconds_ /
                              (60 * 60 * kMillisecondsPerSecond));
}

int Duration::InMinutes() const {
  return SaturatingTrunc<int>(milliseconds_ / (60 * kMillisecondsPerSecond));
}

double Duration::InSecondsF() const {
  return milliseconds_ / kMillisecondsPerSecond;
}

int64_t Duration::InSeconds() const {
  return SaturatingTrunc<int64_t>(InSecondsF());
}

double Duration::InMillisecondsF() const {
  return milliseconds_;
}

int64_t Duration::InMilliseconds() const {
  return SaturatingTrunc<int64_t>(milliseconds_);
}

int64_t Duration::InMillisecondsRoundedUp() const {
  return SaturatingTrunc<int64_t>(std::ceil(milliseconds_));
}

int64_t Duration::InMicroseconds() const {
  return SaturatingTrunc<int64_t>(InMicrosecondsF());
}

double Duration::InMicrosecondsF() const {
  return milliseconds_ * kMicrosecondsPerMillisecond;
}

int64_t Duration::InNanoseconds() const {
  return SaturatingTrunc<int64_t>(InMicrosecondsF() *
                                  kNanosecondsPerMicrosecond);
}

Duration Duration::operator%(Duration other) const {
  return Duration(std::fmod(milliseconds_, other.milliseconds_));
}

namespace base {
namespace time_internal {

int64_t SaturatedAdd(Duration delta, int64_t value) {
  const int64_t a = SaturatingTrunc<int64_t>(delta.InMicrosecondsF());
  if (a > 0 && value > 0 && a > std::numeric_limits<int64_t>::max() - value)
    return std::numeric_limits<int64_t>::max();
  if (a < 0 && value < 0 && a < std::numeric_limits<int64_t>::min() - value)
    return std::numeric_limits<int64_t>::min();
  return a + value;
}

int64_t SaturatedSub(Duration delta, int64_t value) {
  const int64_t a = SaturatingTrunc<int64_t>(delta.InMicrosecondsF());
  if (a > 0 && value < 0 && a > std::numeric_limits<int64_t>::max() + value)
    return std::numeric_limits<int64_t>::max();
  if (a < 0 && value > 0 && a < std::numeric_limits<int64_t>::min() + value)
    return std::numeric_limits<int64_t>::min();
  return a - value;
}

}  // namespace time_internal
}  // namespace base

std::ostream& operator<<(std::ostream& os, Duration duration) {
  return os << duration.InSecondsF() << " s";
}

}  // namespace opcua
