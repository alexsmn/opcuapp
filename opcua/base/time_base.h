#pragma once

// CRTP base shared by opcua::DateTime and opcua::base::TimeTicks: a
// microsecond count (int64_t) with saturating Duration arithmetic. Ported
// from Chromium's base::time_internal::TimeBase.

#include <stdint.h>

#include <limits>

#include "opcua/types/duration.h"

namespace opcua {
namespace base {
namespace time_internal {

template <class TimeClass>
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

  bool is_null() const { return us_ == 0; }
  bool is_max() const { return us_ == std::numeric_limits<int64_t>::max(); }
  bool is_min() const { return us_ == std::numeric_limits<int64_t>::min(); }

  static TimeClass Max() {
    return TimeClass(std::numeric_limits<int64_t>::max());
  }
  static TimeClass Min() {
    return TimeClass(std::numeric_limits<int64_t>::min());
  }

  int64_t ToInternalValue() const { return us_; }

  Duration since_origin() const { return Duration::FromMicroseconds(us_); }

  TimeClass& operator=(TimeClass other) {
    us_ = other.us_;
    return *(static_cast<TimeClass*>(this));
  }

  Duration operator-(TimeClass other) const {
    return Duration::FromMicroseconds(us_ - other.us_);
  }

  TimeClass operator+(Duration delta) const {
    return TimeClass(time_internal::SaturatedAdd(delta, us_));
  }
  TimeClass operator-(Duration delta) const {
    return TimeClass(-time_internal::SaturatedSub(delta, us_));
  }

  TimeClass& operator+=(Duration delta) {
    return static_cast<TimeClass&>(*this = (*this + delta));
  }
  TimeClass& operator-=(Duration delta) {
    return static_cast<TimeClass&>(*this = (*this - delta));
  }

  bool operator==(TimeClass other) const { return us_ == other.us_; }
  bool operator!=(TimeClass other) const { return us_ != other.us_; }
  bool operator<(TimeClass other) const { return us_ < other.us_; }
  bool operator<=(TimeClass other) const { return us_ <= other.us_; }
  bool operator>(TimeClass other) const { return us_ > other.us_; }
  bool operator>=(TimeClass other) const { return us_ >= other.us_; }

 protected:
  constexpr explicit TimeBase(int64_t us) : us_(us) {}

  int64_t us_;
};

}  // namespace time_internal
}  // namespace base

template <class TimeClass>
inline TimeClass operator+(Duration delta, TimeClass t) {
  return t + delta;
}

}  // namespace opcua
