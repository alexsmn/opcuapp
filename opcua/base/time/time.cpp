#include "opcua/base/time/time.h"

#include <cassert>
#include <cmath>
#include <format>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <string_view>

namespace opcua {
namespace base {

// TimeDelta ------------------------------------------------------------------

int TimeDelta::InDays() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(delta_ / Time::kMicrosecondsPerDay);
}

int TimeDelta::InDaysFloored() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  int result = static_cast<int>(delta_ / Time::kMicrosecondsPerDay);
  int64_t remainder = delta_ - (result * Time::kMicrosecondsPerDay);
  if (remainder < 0)
    --result;
  return result;
}

int TimeDelta::InHours() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(delta_ / Time::kMicrosecondsPerHour);
}

int TimeDelta::InMinutes() const {
  if (is_max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(delta_ / Time::kMicrosecondsPerMinute);
}

double TimeDelta::InSecondsF() const {
  if (is_max())
    return std::numeric_limits<double>::infinity();
  return static_cast<double>(delta_) / Time::kMicrosecondsPerSecond;
}

int64_t TimeDelta::InSeconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_ / Time::kMicrosecondsPerSecond;
}

double TimeDelta::InMillisecondsF() const {
  if (is_max())
    return std::numeric_limits<double>::infinity();
  return static_cast<double>(delta_) / Time::kMicrosecondsPerMillisecond;
}

int64_t TimeDelta::InMilliseconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_ / Time::kMicrosecondsPerMillisecond;
}

int64_t TimeDelta::InMillisecondsRoundedUp() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  int64_t result = delta_ / Time::kMicrosecondsPerMillisecond;
  int64_t remainder = delta_ - (result * Time::kMicrosecondsPerMillisecond);
  if (remainder > 0)
    ++result;
  return result;
}

int64_t TimeDelta::InMicroseconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_;
}

double TimeDelta::InMicrosecondsF() const {
  if (is_max())
    return std::numeric_limits<double>::infinity();
  return static_cast<double>(delta_);
}

int64_t TimeDelta::InNanoseconds() const {
  if (is_max())
    return std::numeric_limits<int64_t>::max();
  return delta_ * Time::kNanosecondsPerMicrosecond;
}

namespace time_internal {

int64_t SaturatedAdd(TimeDelta delta, int64_t value) {
  int64_t a = delta.delta_;
  // Check for overflow: both positive and sum wraps negative.
  if (a > 0 && value > 0 && a > std::numeric_limits<int64_t>::max() - value)
    return std::numeric_limits<int64_t>::max();
  // Check for underflow: both negative and sum wraps positive.
  if (a < 0 && value < 0 && a < std::numeric_limits<int64_t>::min() - value)
    return std::numeric_limits<int64_t>::min();
  return a + value;
}

int64_t SaturatedSub(TimeDelta delta, int64_t value) {
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

std::ostream& operator<<(std::ostream& os, TimeDelta time_delta) {
  return os << time_delta.InSecondsF() << " s";
}

// Time -----------------------------------------------------------------------

Time Time::FromDeltaSinceWindowsEpoch(TimeDelta delta) {
  return Time(delta.InMicroseconds());
}

TimeDelta Time::ToDeltaSinceWindowsEpoch() const {
  return TimeDelta::FromMicroseconds(us_);
}

Time Time::FromDoubleT(double dt) {
  if (dt == 0 || std::isnan(dt))
    return Time();
  return Time(static_cast<int64_t>(dt * kMicrosecondsPerSecond) +
              kTimeTToMicrosecondsOffset);
}

double Time::ToDoubleT() const {
  if (is_null())
    return 0;
  return static_cast<double>(us_ - kTimeTToMicrosecondsOffset) /
         kMicrosecondsPerSecond;
}

Time Time::UnixEpoch() {
  Time time;
  time.us_ = kTimeTToMicrosecondsOffset;
  return time;
}

Time Time::LocalMidnight() const {
  Exploded exploded;
  LocalExplode(&exploded);
  exploded.hour = 0;
  exploded.minute = 0;
  exploded.second = 0;
  exploded.millisecond = 0;
  Time out_time;
  if (FromLocalExploded(exploded, &out_time))
    return out_time;
  assert(false && "LocalMidnight failed");
  return Time();
}

bool Time::FromStringInternal(const char* time_string,
                              bool is_local,
                              Time* parsed_time) {
  std::tm tm = {};
  std::string_view input(time_string);
  bool has_timezone = false;
  bool is_utc = false;

  // Skip optional day-of-week prefix like "Tue, ".
  if (input.size() > 4 && input[3] == ',') {
    input.remove_prefix(4);
    while (!input.empty() && input.front() == ' ')
      input.remove_prefix(1);
  }

  std::istringstream ss{std::string(input)};

  // Try ISO 8601 with 'T': "2021-11-07T12:41:21"
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (ss.fail()) {
    // Try ISO 8601 with space: "2004-11-15 10:00:00"
    ss.clear();
    ss.str(std::string(input));
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
  }
  if (ss.fail()) {
    // Try RFC 822-like: "15 Nov 2004 10:00:00 [UTC|GMT]"
    ss.clear();
    ss.str(std::string(input));
    ss >> std::get_time(&tm, "%d %b %Y %H:%M:%S");
  }

  if (ss.fail())
    return false;

  // Parse optional fractional seconds: ".500"
  int millisecond = 0;
  if (ss.peek() == '.') {
    ss.get();
    int frac = 0;
    int digits = 0;
    while (ss.peek() >= '0' && ss.peek() <= '9' && digits < 3) {
      frac = frac * 10 + (ss.get() - '0');
      ++digits;
    }
    // Pad to 3 digits (e.g., ".5" -> 500, ".50" -> 500).
    for (; digits < 3; ++digits)
      frac *= 10;
    millisecond = frac;
    // Skip remaining fractional digits.
    while (ss.peek() >= '0' && ss.peek() <= '9')
      ss.get();
  }

  // Check for timezone suffix.
  std::string tz;
  ss >> tz;
  if (tz == "GMT" || tz == "UTC") {
    has_timezone = true;
    is_utc = true;
  }

  // Determine if this should be treated as UTC.
  // If is_local is false (FromUTCString), always treat as UTC.
  // If is_local is true (FromString), treat as UTC only if timezone says so.
  bool treat_as_utc = !is_local || (has_timezone && is_utc);

  Exploded exploded = {};
  exploded.year = tm.tm_year + 1900;
  exploded.month = tm.tm_mon + 1;
  exploded.day_of_month = tm.tm_mday;
  exploded.hour = tm.tm_hour;
  exploded.minute = tm.tm_min;
  exploded.second = tm.tm_sec;
  exploded.millisecond = millisecond;

  if (treat_as_utc)
    return FromUTCExploded(exploded, parsed_time);
  else
    return FromLocalExploded(exploded, parsed_time);
}

bool Time::ExplodedMostlyEquals(const Exploded& lhs, const Exploded& rhs) {
  return lhs.year == rhs.year && lhs.month == rhs.month &&
         lhs.day_of_month == rhs.day_of_month && lhs.hour == rhs.hour &&
         lhs.minute == rhs.minute && lhs.second == rhs.second &&
         lhs.millisecond == rhs.millisecond;
}

std::ostream& operator<<(std::ostream& os, Time time) {
  Time::Exploded exploded;
  time.UTCExplode(&exploded);
  return os << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} UTC",
                           exploded.year, exploded.month, exploded.day_of_month,
                           exploded.hour, exploded.minute, exploded.second,
                           exploded.millisecond);
}

inline bool is_in_range(int value, int lo, int hi) {
  return lo <= value && value <= hi;
}

bool Time::Exploded::HasValidValues() const {
  return is_in_range(month, 1, 12) && is_in_range(day_of_week, 0, 6) &&
         is_in_range(day_of_month, 1, 31) && is_in_range(hour, 0, 23) &&
         is_in_range(minute, 0, 59) && is_in_range(second, 0, 60) &&
         is_in_range(millisecond, 0, 999);
}

// TimeTicks ------------------------------------------------------------------

std::ostream& operator<<(std::ostream& os, TimeTicks time_ticks) {
  const TimeDelta as_time_delta = time_ticks - TimeTicks();
  return os << as_time_delta.InMicroseconds() << " bogo-microseconds";
}

}  // namespace base
}  // namespace opcua
