#pragma once

// Standalone re-implementation of Chromium's opcua::base::Time, opcua::base::TimeDelta, and
// opcua::base::TimeTicks.  API-compatible with the original so that existing code
// (opcua::scada::DateTime = opcua::base::Time, opcua::scada::Duration = opcua::base::TimeDelta) continues
// to compile unchanged.
//
// Internal representation: microseconds (int64_t).
//   Time       — microseconds since Windows epoch 1601-01-01 00:00:00 UTC.
//   TimeDelta  — signed duration in microseconds.
//   TimeTicks  — monotonic clock, microseconds since an arbitrary origin.

#include <stdint.h>

#include <cassert>
#include <iosfwd>
#include <limits>

namespace opcua {
#ifdef _WIN32
// Forward-declare FILETIME to avoid including <windows.h>.
typedef struct _FILETIME FILETIME;
#endif

namespace base {

class TimeDelta;

namespace time_internal {

int64_t SaturatedAdd(TimeDelta delta, int64_t value);
int64_t SaturatedSub(TimeDelta delta, int64_t value);

}  // namespace time_internal

// TimeDelta ------------------------------------------------------------------

class TimeDelta {
 public:
  constexpr TimeDelta() : delta_(0) {}

  static constexpr TimeDelta FromDays(int days);
  static constexpr TimeDelta FromHours(int hours);
  static constexpr TimeDelta FromMinutes(int minutes);
  static constexpr TimeDelta FromSeconds(int64_t secs);
  static constexpr TimeDelta FromMilliseconds(int64_t ms);
  static constexpr TimeDelta FromMicroseconds(int64_t us);
  static constexpr TimeDelta FromNanoseconds(int64_t ns);
  static constexpr TimeDelta FromSecondsD(double secs);
  static constexpr TimeDelta FromMillisecondsD(double ms);
  static constexpr TimeDelta FromMicrosecondsD(double us);
  static constexpr TimeDelta FromNanosecondsD(double ns);

  static constexpr TimeDelta FromInternalValue(int64_t delta) {
    return TimeDelta(delta);
  }

  static constexpr TimeDelta Max();
  static constexpr TimeDelta Min();

  constexpr int64_t ToInternalValue() const { return delta_; }

  constexpr TimeDelta magnitude() const {
    return TimeDelta(delta_ < 0 ? -delta_ : delta_);
  }

  constexpr bool is_zero() const { return delta_ == 0; }

  constexpr bool is_max() const {
    return delta_ == std::numeric_limits<int64_t>::max();
  }
  constexpr bool is_min() const {
    return delta_ == std::numeric_limits<int64_t>::min();
  }

  int InDays() const;
  int InDaysFloored() const;
  int InHours() const;
  int InMinutes() const;
  double InSecondsF() const;
  int64_t InSeconds() const;
  double InMillisecondsF() const;
  int64_t InMilliseconds() const;
  int64_t InMillisecondsRoundedUp() const;
  int64_t InMicroseconds() const;
  double InMicrosecondsF() const;
  int64_t InNanoseconds() const;

  constexpr TimeDelta& operator=(TimeDelta other) {
    delta_ = other.delta_;
    return *this;
  }

  TimeDelta operator+(TimeDelta other) const {
    return TimeDelta(time_internal::SaturatedAdd(*this, other.delta_));
  }
  TimeDelta operator-(TimeDelta other) const {
    return TimeDelta(time_internal::SaturatedSub(*this, other.delta_));
  }

  TimeDelta& operator+=(TimeDelta other) { return *this = (*this + other); }
  TimeDelta& operator-=(TimeDelta other) { return *this = (*this - other); }
  constexpr TimeDelta operator-() const { return TimeDelta(-delta_); }

  template <typename T>
  TimeDelta operator*(T a) const {
    // Simple overflow-saturating multiply.
    if (a == 0 || delta_ == 0)
      return TimeDelta(0);
    int64_t result;
#if defined(_MSC_VER)
    // MSVC doesn't have __int128; use manual check.
    if (delta_ > 0 && a > 0 &&
        delta_ > std::numeric_limits<int64_t>::max() / a)
      return Max();
    if (delta_ > 0 && a < 0 &&
        a < std::numeric_limits<int64_t>::min() / delta_)
      return Min();
    if (delta_ < 0 && a > 0 &&
        delta_ < std::numeric_limits<int64_t>::min() / a)
      return Min();
    if (delta_ < 0 && a < 0 &&
        delta_ < std::numeric_limits<int64_t>::max() / a)
      return Max();
    result = delta_ * static_cast<int64_t>(a);
#else
    __int128 big = static_cast<__int128>(delta_) * static_cast<int64_t>(a);
    if (big > std::numeric_limits<int64_t>::max())
      return Max();
    if (big < std::numeric_limits<int64_t>::min())
      return Min();
    result = static_cast<int64_t>(big);
#endif
    return TimeDelta(result);
  }

  template <typename T>
  constexpr TimeDelta operator/(T a) const {
    return TimeDelta(delta_ / static_cast<int64_t>(a));
  }

  template <typename T>
  TimeDelta& operator*=(T a) {
    return *this = (*this * a);
  }
  template <typename T>
  constexpr TimeDelta& operator/=(T a) {
    return *this = (*this / a);
  }

  constexpr int64_t operator/(TimeDelta a) const { return delta_ / a.delta_; }
  constexpr TimeDelta operator%(TimeDelta a) const {
    return TimeDelta(delta_ % a.delta_);
  }

  constexpr bool operator==(TimeDelta other) const {
    return delta_ == other.delta_;
  }
  constexpr bool operator!=(TimeDelta other) const {
    return delta_ != other.delta_;
  }
  constexpr bool operator<(TimeDelta other) const {
    return delta_ < other.delta_;
  }
  constexpr bool operator<=(TimeDelta other) const {
    return delta_ <= other.delta_;
  }
  constexpr bool operator>(TimeDelta other) const {
    return delta_ > other.delta_;
  }
  constexpr bool operator>=(TimeDelta other) const {
    return delta_ >= other.delta_;
  }

 private:
  friend int64_t time_internal::SaturatedAdd(TimeDelta delta, int64_t value);
  friend int64_t time_internal::SaturatedSub(TimeDelta delta, int64_t value);

  constexpr explicit TimeDelta(int64_t delta_us) : delta_(delta_us) {}

  static constexpr TimeDelta FromDouble(double value);
  static constexpr TimeDelta FromProduct(int64_t value, int64_t positive_value);

  int64_t delta_;
};

template <typename T>
TimeDelta operator*(T a, TimeDelta td) {
  return td * a;
}

std::ostream& operator<<(std::ostream& os, TimeDelta time_delta);

// TimeBase -------------------------------------------------------------------

namespace time_internal {

template <class TimeClass>
class TimeBase {
 public:
  static constexpr int64_t kHoursPerDay = 24;
  static constexpr int64_t kMillisecondsPerSecond = 1000;
  static constexpr int64_t kMillisecondsPerDay =
      kMillisecondsPerSecond * 60 * 60 * kHoursPerDay;
  static constexpr int64_t kMicrosecondsPerMillisecond = 1000;
  static constexpr int64_t kMicrosecondsPerSecond =
      kMicrosecondsPerMillisecond * kMillisecondsPerSecond;
  static constexpr int64_t kMicrosecondsPerMinute = kMicrosecondsPerSecond * 60;
  static constexpr int64_t kMicrosecondsPerHour = kMicrosecondsPerMinute * 60;
  static constexpr int64_t kMicrosecondsPerDay =
      kMicrosecondsPerHour * kHoursPerDay;
  static constexpr int64_t kMicrosecondsPerWeek = kMicrosecondsPerDay * 7;
  static constexpr int64_t kNanosecondsPerMicrosecond = 1000;
  static constexpr int64_t kNanosecondsPerSecond =
      kNanosecondsPerMicrosecond * kMicrosecondsPerSecond;

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

  TimeDelta since_origin() const { return TimeDelta::FromMicroseconds(us_); }

  TimeClass& operator=(TimeClass other) {
    us_ = other.us_;
    return *(static_cast<TimeClass*>(this));
  }

  TimeDelta operator-(TimeClass other) const {
    return TimeDelta::FromMicroseconds(us_ - other.us_);
  }

  TimeClass operator+(TimeDelta delta) const {
    return TimeClass(time_internal::SaturatedAdd(delta, us_));
  }
  TimeClass operator-(TimeDelta delta) const {
    return TimeClass(-time_internal::SaturatedSub(delta, us_));
  }

  TimeClass& operator+=(TimeDelta delta) {
    return static_cast<TimeClass&>(*this = (*this + delta));
  }
  TimeClass& operator-=(TimeDelta delta) {
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

template <class TimeClass>
inline TimeClass operator+(TimeDelta delta, TimeClass t) {
  return t + delta;
}

// Time -----------------------------------------------------------------------

class Time : public time_internal::TimeBase<Time> {
 public:
  static constexpr int64_t kTimeTToMicrosecondsOffset =
      INT64_C(11644473600000000);

  struct Exploded {
    int year;
    int month;
    int day_of_week;
    int day_of_month;
    int hour;
    int minute;
    int second;
    int millisecond;

    bool HasValidValues() const;
  };

  constexpr Time() : TimeBase(0) {}

  static Time UnixEpoch();
  static Time Now();

  static Time FromDeltaSinceWindowsEpoch(TimeDelta delta);
  TimeDelta ToDeltaSinceWindowsEpoch() const;

  static Time FromDoubleT(double dt);
  double ToDoubleT() const;

  static bool FromUTCExploded(const Exploded& exploded, Time* time) {
    return FromExploded(false, exploded, time);
  }
  static bool FromLocalExploded(const Exploded& exploded, Time* time) {
    return FromExploded(true, exploded, time);
  }

  static bool FromString(const char* time_string, Time* parsed_time) {
    return FromStringInternal(time_string, true, parsed_time);
  }
  static bool FromUTCString(const char* time_string, Time* parsed_time) {
    return FromStringInternal(time_string, false, parsed_time);
  }

  void UTCExplode(Exploded* exploded) const { Explode(false, exploded); }
  void LocalExplode(Exploded* exploded) const { Explode(true, exploded); }

  Time LocalMidnight() const;

#ifdef _WIN32
  // FILETIME uses 100-nanosecond intervals since the same Windows epoch.
  static Time FromFileTime(FILETIME ft);
  FILETIME ToFileTime() const;
#endif

  static constexpr Time FromInternalValue(int64_t us) { return Time(us); }

 private:
  friend class time_internal::TimeBase<Time>;

  constexpr explicit Time(int64_t us) : TimeBase(us) {}

  void Explode(bool is_local, Exploded* exploded) const;
  static bool FromExploded(bool is_local,
                           const Exploded& exploded,
                           Time* time);
  static bool FromStringInternal(const char* time_string,
                                 bool is_local,
                                 Time* parsed_time);
  static bool ExplodedMostlyEquals(const Exploded& lhs, const Exploded& rhs);
};

// Constexpr TimeDelta factory implementations --------------------------------

constexpr TimeDelta TimeDelta::FromDays(int days) {
  return days == std::numeric_limits<int>::max()
             ? Max()
             : TimeDelta(days * Time::kMicrosecondsPerDay);
}

constexpr TimeDelta TimeDelta::FromHours(int hours) {
  return hours == std::numeric_limits<int>::max()
             ? Max()
             : TimeDelta(hours * Time::kMicrosecondsPerHour);
}

constexpr TimeDelta TimeDelta::FromMinutes(int minutes) {
  return minutes == std::numeric_limits<int>::max()
             ? Max()
             : TimeDelta(minutes * Time::kMicrosecondsPerMinute);
}

constexpr TimeDelta TimeDelta::FromSeconds(int64_t secs) {
  return FromProduct(secs, Time::kMicrosecondsPerSecond);
}

constexpr TimeDelta TimeDelta::FromMilliseconds(int64_t ms) {
  return FromProduct(ms, Time::kMicrosecondsPerMillisecond);
}

constexpr TimeDelta TimeDelta::FromMicroseconds(int64_t us) {
  return TimeDelta(us);
}

constexpr TimeDelta TimeDelta::FromNanoseconds(int64_t ns) {
  return TimeDelta(ns / Time::kNanosecondsPerMicrosecond);
}

constexpr TimeDelta TimeDelta::FromSecondsD(double secs) {
  return FromDouble(secs * Time::kMicrosecondsPerSecond);
}

constexpr TimeDelta TimeDelta::FromMillisecondsD(double ms) {
  return FromDouble(ms * Time::kMicrosecondsPerMillisecond);
}

constexpr TimeDelta TimeDelta::FromMicrosecondsD(double us) {
  return FromDouble(us);
}

constexpr TimeDelta TimeDelta::FromNanosecondsD(double ns) {
  return FromDouble(ns / Time::kNanosecondsPerMicrosecond);
}

constexpr TimeDelta TimeDelta::Max() {
  return TimeDelta(std::numeric_limits<int64_t>::max());
}

constexpr TimeDelta TimeDelta::Min() {
  return TimeDelta(std::numeric_limits<int64_t>::min());
}

constexpr TimeDelta TimeDelta::FromDouble(double value) {
  return value > static_cast<double>(std::numeric_limits<int64_t>::max())
             ? Max()
             : value < static_cast<double>(std::numeric_limits<int64_t>::min())
                   ? Min()
                   : TimeDelta(static_cast<int64_t>(value));
}

constexpr TimeDelta TimeDelta::FromProduct(int64_t value,
                                           int64_t positive_value) {
  return value > std::numeric_limits<int64_t>::max() / positive_value
             ? Max()
             : value < std::numeric_limits<int64_t>::min() / positive_value
                   ? Min()
                   : TimeDelta(value * positive_value);
}

std::ostream& operator<<(std::ostream& os, Time time);

// TimeTicks ------------------------------------------------------------------

class TimeTicks : public time_internal::TimeBase<TimeTicks> {
 public:
  constexpr TimeTicks() : TimeBase(0) {}

  static TimeTicks Now();

  static constexpr TimeTicks FromInternalValue(int64_t us) {
    return TimeTicks(us);
  }

 private:
  friend class time_internal::TimeBase<TimeTicks>;

  constexpr explicit TimeTicks(int64_t us) : TimeBase(us) {}
};

std::ostream& operator<<(std::ostream& os, TimeTicks time_ticks);

}  // namespace base
}  // namespace opcua (vendored)
