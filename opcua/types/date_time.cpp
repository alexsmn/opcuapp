#include "opcua/types/date_time.h"

#include <cassert>
#include <cmath>
#include <format>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string_view>

#include "opcua/base/format_time.h"
#include "opcua/base/utf_convert.h"

namespace opcua {

DateTime DateTime::FromDeltaSinceWindowsEpoch(Duration delta) {
  return DateTime(delta.InMicroseconds());
}

Duration DateTime::ToDeltaSinceWindowsEpoch() const {
  return Duration::FromMicroseconds(us_);
}

DateTime DateTime::FromDoubleT(double dt) {
  if (dt == 0 || std::isnan(dt))
    return DateTime();
  return DateTime(static_cast<int64_t>(dt * kMicrosecondsPerSecond) +
                  kTimeTToMicrosecondsOffset);
}

double DateTime::ToDoubleT() const {
  if (is_null())
    return 0;
  return static_cast<double>(us_ - kTimeTToMicrosecondsOffset) /
         kMicrosecondsPerSecond;
}

DateTime DateTime::UnixEpoch() {
  DateTime time;
  time.us_ = kTimeTToMicrosecondsOffset;
  return time;
}

DateTime DateTime::LocalMidnight() const {
  Exploded exploded;
  LocalExplode(&exploded);
  exploded.hour = 0;
  exploded.minute = 0;
  exploded.second = 0;
  exploded.millisecond = 0;
  DateTime out_time;
  if (FromLocalExploded(exploded, &out_time))
    return out_time;
  assert(false && "LocalMidnight failed");
  return DateTime();
}

bool DateTime::FromStringInternal(const char* time_string,
                                  bool is_local,
                                  DateTime* parsed_time) {
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

bool DateTime::ExplodedMostlyEquals(const Exploded& lhs, const Exploded& rhs) {
  return lhs.year == rhs.year && lhs.month == rhs.month &&
         lhs.day_of_month == rhs.day_of_month && lhs.hour == rhs.hour &&
         lhs.minute == rhs.minute && lhs.second == rhs.second &&
         lhs.millisecond == rhs.millisecond;
}

std::ostream& operator<<(std::ostream& os, DateTime time) {
  DateTime::Exploded exploded;
  time.UTCExplode(&exploded);
  return os << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} UTC",
                           exploded.year, exploded.month, exploded.day_of_month,
                           exploded.hour, exploded.minute, exploded.second,
                           exploded.millisecond);
}

namespace {

inline bool is_in_range(int value, int lo, int hi) {
  return lo <= value && value <= hi;
}

}  // namespace

bool DateTime::Exploded::HasValidValues() const {
  return is_in_range(month, 1, 12) && is_in_range(day_of_week, 0, 6) &&
         is_in_range(day_of_month, 1, 31) && is_in_range(hour, 0, 23) &&
         is_in_range(minute, 0, 59) && is_in_range(second, 0, 60) &&
         is_in_range(millisecond, 0, 999);
}

std::string ToString(DateTime time) {
  return FormatTime(time);
}

std::u16string ToString16(DateTime time) {
  return UtfConvert<char16_t>(FormatTime(time));
}

}  // namespace opcua
