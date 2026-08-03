#pragma once

// CRTP base shared by opcua::DateTime and opcua::base::TimeTicks. The storage
// unit is selected by TicksPerMicrosecond and Duration arithmetic saturates.
// Ported from Chromium's base::time_internal::TimeBase.

#include <stdint.h>

#include <limits>

#include "opcua/types/duration.h"

namespace opcua {
namespace base {
namespace time_internal {

template <class TimeClass, int64_t TicksPerMicrosecond = 1>
class TimeBase {
 public:
  // Unit constants re-exported from Duration so existing spellings like
  // DateTime::kMicrosecondsPerSecond keep working.
  static constexpr int64_t kHoursPerDay = Duration::kHoursPerDay;
  static constexpr int64_t kMillisecondsPerSecond =
      Duration::kMillisecondsPerSecond;
  static constexpr int64_t kMillisecondsPerDay = Duration::kMillisecondsPerDay;
  static constexpr int64_t kMicrosecondsPerMillisecond =
      Duration::kMicrosecondsPerMillisecond;
  static constexpr int64_t kMicrosecondsPerSecond =
      Duration::kMicrosecondsPerSecond;
  static constexpr int64_t kMicrosecondsPerMinute =
      Duration::kMicrosecondsPerMinute;
  static constexpr int64_t kMicrosecondsPerHour =
      Duration::kMicrosecondsPerHour;
  static constexpr int64_t kMicrosecondsPerDay = Duration::kMicrosecondsPerDay;
  static constexpr int64_t kMicrosecondsPerWeek =
      Duration::kMicrosecondsPerWeek;
  static constexpr int64_t kNanosecondsPerMicrosecond =
      Duration::kNanosecondsPerMicrosecond;
  static constexpr int64_t kNanosecondsPerSecond =
      Duration::kNanosecondsPerSecond;

  bool is_null() const { return ticks_ == 0; }
  bool is_max() const { return ticks_ == std::numeric_limits<int64_t>::max(); }
  bool is_min() const { return ticks_ == std::numeric_limits<int64_t>::min(); }

  static TimeClass Max() {
    return TimeClass(std::numeric_limits<int64_t>::max());
  }
  static TimeClass Min() {
    return TimeClass(std::numeric_limits<int64_t>::min());
  }

  int64_t ToInternalValue() const { return ticks_; }

  Duration since_origin() const {
    return Duration::FromMicrosecondsD(static_cast<double>(ticks_) /
                                       TicksPerMicrosecond);
  }

  TimeClass& operator=(TimeClass other) {
    ticks_ = other.ticks_;
    return *(static_cast<TimeClass*>(this));
  }

  Duration operator-(TimeClass other) const {
    return Duration::FromMicrosecondsD(
        (static_cast<double>(ticks_) - static_cast<double>(other.ticks_)) /
        TicksPerMicrosecond);
  }

  TimeClass operator+(Duration delta) const {
    return TimeClass(
        time_internal::SaturatedAdd(delta * TicksPerMicrosecond, ticks_));
  }
  TimeClass operator-(Duration delta) const {
    return TimeClass(
        -time_internal::SaturatedSub(delta * TicksPerMicrosecond, ticks_));
  }

  TimeClass& operator+=(Duration delta) {
    return static_cast<TimeClass&>(*this = (*this + delta));
  }
  TimeClass& operator-=(Duration delta) {
    return static_cast<TimeClass&>(*this = (*this - delta));
  }

  bool operator==(TimeClass other) const { return ticks_ == other.ticks_; }
  bool operator!=(TimeClass other) const { return ticks_ != other.ticks_; }
  bool operator<(TimeClass other) const { return ticks_ < other.ticks_; }
  bool operator<=(TimeClass other) const { return ticks_ <= other.ticks_; }
  bool operator>(TimeClass other) const { return ticks_ > other.ticks_; }
  bool operator>=(TimeClass other) const { return ticks_ >= other.ticks_; }

 protected:
  constexpr explicit TimeBase(int64_t ticks) : ticks_(ticks) {}

  int64_t ticks_;
};

}  // namespace time_internal
}  // namespace base

template <class TimeClass>
inline TimeClass operator+(Duration delta, TimeClass t) {
  return t + delta;
}

}  // namespace opcua
