#include "opcua/types/duration.h"

#include <limits>
#include <ostream>

namespace opcua {

int Duration::InDays() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(delta_ / kMicrosecondsPerDay);
}

int Duration::InDaysFloored() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  int result = static_cast<int>(delta_ / kMicrosecondsPerDay);
  int64_t remainder = delta_ - (result * kMicrosecondsPerDay);
  if (remainder < 0)
    --result;
  return result;
}

int Duration::InHours() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(delta_ / kMicrosecondsPerHour);
}

int Duration::InMinutes() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(delta_ / kMicrosecondsPerMinute);
}

double Duration::InSecondsF() const {
  if (is_max())
    return std::numeric_limits<double>::infinity();
  return static_cast<double>(delta_) / kMicrosecondsPerSecond;
}

int64_t Duration::InSeconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_ / kMicrosecondsPerSecond;
}

double Duration::InMillisecondsF() const {
  if (is_max())
    return std::numeric_limits<double>::infinity();
  return static_cast<double>(delta_) / kMicrosecondsPerMillisecond;
}

int64_t Duration::InMilliseconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_ / kMicrosecondsPerMillisecond;
}

int64_t Duration::InMillisecondsRoundedUp() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  int64_t result = delta_ / kMicrosecondsPerMillisecond;
  int64_t remainder = delta_ - (result * kMicrosecondsPerMillisecond);
  if (remainder > 0)
    ++result;
  return result;
}

int64_t Duration::InMicroseconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_;
}

double Duration::InMicrosecondsF() const {
  if (is_max())
    return std::numeric_limits<double>::infinity();
  return static_cast<double>(delta_);
}

int64_t Duration::InNanoseconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_ * kNanosecondsPerMicrosecond;
}

namespace base {
namespace time_internal {

int64_t SaturatedAdd(Duration delta, int64_t value) {
  int64_t a = delta.delta_;
  // Check for overflow: both positive and sum wraps negative.
  if (a > 0 && value > 0 && a > std::numeric_limits<int64_t>::max() - value)
    return std::numeric_limits<int64_t>::max();
  // Check for underflow: both negative and sum wraps positive.
  if (a < 0 && value < 0 && a < std::numeric_limits<int64_t>::min() - value)
    return std::numeric_limits<int64_t>::min();
  return a + value;
}

int64_t SaturatedSub(Duration delta, int64_t value) {
  int64_t a = delta.delta_;
  // Check for overflow: a positive, value negative, result wraps.
  if (a > 0 && value < 0 && a > std::numeric_limits<int64_t>::max() + value)
    return std::numeric_limits<int64_t>::max();
  // Check for underflow: a negative, value positive, result wraps.
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
