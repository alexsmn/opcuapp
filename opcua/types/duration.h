#pragma once

// Standalone re-implementation of Chromium's TimeDelta (renamed
// opcua::Duration here). API-compatible with the Chromium original so that
// ported code continues to compile unchanged.
//
// Internal representation: a signed, saturating microsecond count (int64_t).

#include <stdint.h>

#include <iosfwd>
#include <limits>

namespace opcua {

class Duration;

namespace base {
namespace time_internal {

int64_t SaturatedAdd(Duration delta, int64_t value);
int64_t SaturatedSub(Duration delta, int64_t value);

}  // namespace time_internal
}  // namespace base

// OPC UA Duration: a time interval in milliseconds (a Double subtype), here
// carried as a microsecond count. OPC UA Part 3 §8.13 Duration,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.13
class Duration {
 public:
  // Time unit conversion factors. Also re-exported by
  // base::time_internal::TimeBase so DateTime::kMicrosecondsPerSecond etc.
  // keep working.
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

  constexpr Duration() : delta_(0) {}

  static constexpr Duration FromDays(int days);
  static constexpr Duration FromHours(int hours);
  static constexpr Duration FromMinutes(int minutes);
  static constexpr Duration FromSeconds(int64_t secs);
  static constexpr Duration FromMilliseconds(int64_t ms);
  static constexpr Duration FromMicroseconds(int64_t us);
  static constexpr Duration FromNanoseconds(int64_t ns);
  static constexpr Duration FromSecondsD(double secs);
  static constexpr Duration FromMillisecondsD(double ms);
  static constexpr Duration FromMicrosecondsD(double us);
  static constexpr Duration FromNanosecondsD(double ns);

  static constexpr Duration FromInternalValue(int64_t delta) {
    return Duration(delta);
  }

  static constexpr Duration Max();
  static constexpr Duration Min();

  constexpr int64_t ToInternalValue() const { return delta_; }

  constexpr Duration magnitude() const {
    return Duration(delta_ < 0 ? -delta_ : delta_);
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

  constexpr Duration& operator=(Duration other) {
    delta_ = other.delta_;
    return *this;
  }

  Duration operator+(Duration other) const {
    return Duration(base::time_internal::SaturatedAdd(*this, other.delta_));
  }
  Duration operator-(Duration other) const {
    return Duration(base::time_internal::SaturatedSub(*this, other.delta_));
  }

  Duration& operator+=(Duration other) { return *this = (*this + other); }
  Duration& operator-=(Duration other) { return *this = (*this - other); }
  constexpr Duration operator-() const { return Duration(-delta_); }

  template <typename T>
  Duration operator*(T a) const {
    // Simple overflow-saturating multiply.
    if (a == 0 || delta_ == 0)
      return Duration(0);
    int64_t result;
#if defined(_MSC_VER)
    // MSVC doesn't have __int128; use manual check.
    if (delta_ > 0 && a > 0 && delta_ > std::numeric_limits<int64_t>::max() / a)
      return Max();
    if (delta_ > 0 && a < 0 && a < std::numeric_limits<int64_t>::min() / delta_)
      return Min();
    if (delta_ < 0 && a > 0 && delta_ < std::numeric_limits<int64_t>::min() / a)
      return Min();
    if (delta_ < 0 && a < 0 && delta_ < std::numeric_limits<int64_t>::max() / a)
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
    return Duration(result);
  }

  template <typename T>
  constexpr Duration operator/(T a) const {
    return Duration(delta_ / static_cast<int64_t>(a));
  }

  template <typename T>
  Duration& operator*=(T a) {
    return *this = (*this * a);
  }
  template <typename T>
  constexpr Duration& operator/=(T a) {
    return *this = (*this / a);
  }

  constexpr int64_t operator/(Duration a) const { return delta_ / a.delta_; }
  constexpr Duration operator%(Duration a) const {
    return Duration(delta_ % a.delta_);
  }

  constexpr bool operator==(Duration other) const {
    return delta_ == other.delta_;
  }
  constexpr bool operator!=(Duration other) const {
    return delta_ != other.delta_;
  }
  constexpr bool operator<(Duration other) const {
    return delta_ < other.delta_;
  }
  constexpr bool operator<=(Duration other) const {
    return delta_ <= other.delta_;
  }
  constexpr bool operator>(Duration other) const {
    return delta_ > other.delta_;
  }
  constexpr bool operator>=(Duration other) const {
    return delta_ >= other.delta_;
  }

 private:
  friend int64_t base::time_internal::SaturatedAdd(Duration delta,
                                                   int64_t value);
  friend int64_t base::time_internal::SaturatedSub(Duration delta,
                                                   int64_t value);

  constexpr explicit Duration(int64_t delta_us) : delta_(delta_us) {}

  static constexpr Duration FromDouble(double value);
  static constexpr Duration FromProduct(int64_t value, int64_t positive_value);

  int64_t delta_;
};

template <typename T>
Duration operator*(T a, Duration td) {
  return td * a;
}

std::ostream& operator<<(std::ostream& os, Duration duration);

// Constexpr Duration factory implementations ---------------------------------

constexpr Duration Duration::FromDays(int days) {
  return days == std::numeric_limits<int>::max()
             ? Max()
             : Duration(days * kMicrosecondsPerDay);
}

constexpr Duration Duration::FromHours(int hours) {
  return hours == std::numeric_limits<int>::max()
             ? Max()
             : Duration(hours * kMicrosecondsPerHour);
}

constexpr Duration Duration::FromMinutes(int minutes) {
  return minutes == std::numeric_limits<int>::max()
             ? Max()
             : Duration(minutes * kMicrosecondsPerMinute);
}

constexpr Duration Duration::FromSeconds(int64_t secs) {
  return FromProduct(secs, kMicrosecondsPerSecond);
}

constexpr Duration Duration::FromMilliseconds(int64_t ms) {
  return FromProduct(ms, kMicrosecondsPerMillisecond);
}

constexpr Duration Duration::FromMicroseconds(int64_t us) {
  return Duration(us);
}

constexpr Duration Duration::FromNanoseconds(int64_t ns) {
  return Duration(ns / kNanosecondsPerMicrosecond);
}

constexpr Duration Duration::FromSecondsD(double secs) {
  return FromDouble(secs * kMicrosecondsPerSecond);
}

constexpr Duration Duration::FromMillisecondsD(double ms) {
  return FromDouble(ms * kMicrosecondsPerMillisecond);
}

constexpr Duration Duration::FromMicrosecondsD(double us) {
  return FromDouble(us);
}

constexpr Duration Duration::FromNanosecondsD(double ns) {
  return FromDouble(ns / kNanosecondsPerMicrosecond);
}

constexpr Duration Duration::Max() {
  return Duration(std::numeric_limits<int64_t>::max());
}

constexpr Duration Duration::Min() {
  return Duration(std::numeric_limits<int64_t>::min());
}

constexpr Duration Duration::FromDouble(double value) {
  return value > static_cast<double>(std::numeric_limits<int64_t>::max())
             ? Max()
         : value < static_cast<double>(std::numeric_limits<int64_t>::min())
             ? Min()
             : Duration(static_cast<int64_t>(value));
}

constexpr Duration Duration::FromProduct(int64_t value,
                                         int64_t positive_value) {
  return value > std::numeric_limits<int64_t>::max() / positive_value ? Max()
         : value < std::numeric_limits<int64_t>::min() / positive_value
             ? Min()
             : Duration(value * positive_value);
}

}  // namespace opcua
