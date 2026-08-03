#pragma once

// Time interval utilities for the OPC UA Duration DataType.

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

// OPC UA Duration: a Double representing a time interval in milliseconds.
// Fractions represent sub-millisecond values. OPC UA Part 3 §8.13 Duration:
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

  constexpr Duration() = default;

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

  // Constructs a Duration from its OPC UA Double millisecond representation.
  static constexpr Duration FromInternalValue(double milliseconds) {
    return Duration(milliseconds);
  }

  static constexpr Duration Max();
  static constexpr Duration Min();

  constexpr double ToInternalValue() const { return milliseconds_; }

  constexpr Duration magnitude() const {
    return Duration(milliseconds_ < 0 ? -milliseconds_ : milliseconds_);
  }

  constexpr bool is_zero() const { return milliseconds_ == 0; }

  constexpr bool is_max() const {
    return milliseconds_ == std::numeric_limits<double>::infinity();
  }
  constexpr bool is_min() const {
    return milliseconds_ == -std::numeric_limits<double>::infinity();
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
    milliseconds_ = other.milliseconds_;
    return *this;
  }

  constexpr Duration operator+(Duration other) const {
    return Duration(milliseconds_ + other.milliseconds_);
  }
  constexpr Duration operator-(Duration other) const {
    return Duration(milliseconds_ - other.milliseconds_);
  }

  constexpr Duration& operator+=(Duration other) {
    return *this = (*this + other);
  }
  constexpr Duration& operator-=(Duration other) {
    return *this = (*this - other);
  }
  constexpr Duration operator-() const { return Duration(-milliseconds_); }

  template <typename T>
  constexpr Duration operator*(T value) const {
    return Duration(milliseconds_ * static_cast<double>(value));
  }

  template <typename T>
  constexpr Duration operator/(T value) const {
    return Duration(milliseconds_ / static_cast<double>(value));
  }

  template <typename T>
  constexpr Duration& operator*=(T value) {
    return *this = (*this * value);
  }
  template <typename T>
  constexpr Duration& operator/=(T value) {
    return *this = (*this / value);
  }

  constexpr double operator/(Duration other) const {
    return milliseconds_ / other.milliseconds_;
  }
  Duration operator%(Duration other) const;

  constexpr bool operator==(Duration other) const {
    return milliseconds_ == other.milliseconds_;
  }
  constexpr bool operator!=(Duration other) const {
    return milliseconds_ != other.milliseconds_;
  }
  constexpr bool operator<(Duration other) const {
    return milliseconds_ < other.milliseconds_;
  }
  constexpr bool operator<=(Duration other) const {
    return milliseconds_ <= other.milliseconds_;
  }
  constexpr bool operator>(Duration other) const {
    return milliseconds_ > other.milliseconds_;
  }
  constexpr bool operator>=(Duration other) const {
    return milliseconds_ >= other.milliseconds_;
  }

 private:
  constexpr explicit Duration(double milliseconds)
      : milliseconds_(milliseconds) {}

  double milliseconds_ = 0;
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
             : Duration(static_cast<double>(days) * kMillisecondsPerDay);
}

constexpr Duration Duration::FromHours(int hours) {
  return hours == std::numeric_limits<int>::max()
             ? Max()
             : Duration(static_cast<double>(hours) * 60 * 60 *
                        kMillisecondsPerSecond);
}

constexpr Duration Duration::FromMinutes(int minutes) {
  return minutes == std::numeric_limits<int>::max()
             ? Max()
             : Duration(static_cast<double>(minutes) * 60 *
                        kMillisecondsPerSecond);
}

constexpr Duration Duration::FromSeconds(int64_t secs) {
  return Duration(static_cast<double>(secs) * kMillisecondsPerSecond);
}

constexpr Duration Duration::FromMilliseconds(int64_t ms) {
  return Duration(static_cast<double>(ms));
}

constexpr Duration Duration::FromMicroseconds(int64_t us) {
  return Duration(static_cast<double>(us) / kMicrosecondsPerMillisecond);
}

constexpr Duration Duration::FromNanoseconds(int64_t ns) {
  return Duration(static_cast<double>(ns) /
                  (kNanosecondsPerMicrosecond * kMicrosecondsPerMillisecond));
}

constexpr Duration Duration::FromSecondsD(double secs) {
  return Duration(secs * kMillisecondsPerSecond);
}

constexpr Duration Duration::FromMillisecondsD(double ms) {
  return Duration(ms);
}

constexpr Duration Duration::FromMicrosecondsD(double us) {
  return Duration(us / kMicrosecondsPerMillisecond);
}

constexpr Duration Duration::FromNanosecondsD(double ns) {
  return Duration(ns /
                  (kNanosecondsPerMicrosecond * kMicrosecondsPerMillisecond));
}

constexpr Duration Duration::Max() {
  return Duration(std::numeric_limits<double>::infinity());
}

constexpr Duration Duration::Min() {
  return Duration(-std::numeric_limits<double>::infinity());
}

}  // namespace opcua
