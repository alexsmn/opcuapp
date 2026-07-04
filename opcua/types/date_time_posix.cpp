#ifndef _WIN32

#include "opcua/types/date_time.h"

#include "opcua/base/test/scoped_mock_clock_override.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sys/time.h>
#include <time.h>

namespace opcua {
namespace {

std::mutex& GetTimeStructLock() {
  static std::mutex lock;
  return lock;
}

time_t SysTimeFromTimeStruct(struct tm* timestruct, bool is_local) {
  std::lock_guard guard(GetTimeStructLock());
  return is_local ? mktime(timestruct) : timegm(timestruct);
}

void SysTimeToTimeStruct(time_t t, struct tm* timestruct, bool is_local) {
  std::lock_guard guard(GetTimeStructLock());
  if (is_local)
    localtime_r(&t, timestruct);
  else
    gmtime_r(&t, timestruct);
}

}  // namespace

DateTime DateTime::Now() {
  if (auto* mock = base::ScopedMockClockOverride::current())
    return mock->Now();
  struct timeval tv;
  struct timezone tz = {0, 0};
  gettimeofday(&tv, &tz);
  const int64_t microseconds = tv.tv_sec * kMicrosecondsPerSecond + tv.tv_usec +
                               kTimeTToMicrosecondsOffset;
  return DateTime::FromInternalValue(microseconds * kTicksPerMicrosecond);
}

void DateTime::Explode(bool is_local, Exploded* exploded) const {
  int64_t microseconds =
      ToDeltaSinceWindowsEpoch().InMicroseconds() - kTimeTToMicrosecondsOffset;

  int64_t milliseconds;
  time_t seconds;
  int millisecond;

  if (microseconds >= 0) {
    milliseconds = microseconds / kMicrosecondsPerMillisecond;
    seconds = static_cast<time_t>(milliseconds / kMillisecondsPerSecond);
    millisecond = static_cast<int>(milliseconds % kMillisecondsPerSecond);
  } else {
    milliseconds = (microseconds - kMicrosecondsPerMillisecond + 1) /
                   kMicrosecondsPerMillisecond;
    seconds = static_cast<time_t>((milliseconds - kMillisecondsPerSecond + 1) /
                                  kMillisecondsPerSecond);
    millisecond = static_cast<int>(milliseconds % kMillisecondsPerSecond);
    if (millisecond < 0)
      millisecond += static_cast<int>(kMillisecondsPerSecond);
  }

  struct tm timestruct;
  SysTimeToTimeStruct(seconds, &timestruct, is_local);

  exploded->year = timestruct.tm_year + 1900;
  exploded->month = timestruct.tm_mon + 1;
  exploded->day_of_week = timestruct.tm_wday;
  exploded->day_of_month = timestruct.tm_mday;
  exploded->hour = timestruct.tm_hour;
  exploded->minute = timestruct.tm_min;
  exploded->second = timestruct.tm_sec;
  exploded->millisecond = millisecond;
}

bool DateTime::FromExploded(bool is_local,
                            const Exploded& exploded,
                            DateTime* time) {
  int month = exploded.month - 1;
  int year = exploded.year - 1900;

  struct tm timestruct;
  std::memset(&timestruct, 0, sizeof(timestruct));
  timestruct.tm_sec = exploded.second;
  timestruct.tm_min = exploded.minute;
  timestruct.tm_hour = exploded.hour;
  timestruct.tm_mday = exploded.day_of_month;
  timestruct.tm_mon = month;
  timestruct.tm_year = year;
  timestruct.tm_wday = exploded.day_of_week;
  timestruct.tm_yday = 0;
  timestruct.tm_isdst = -1;
#if !defined(__sun)
  timestruct.tm_gmtoff = 0;
  timestruct.tm_zone = nullptr;
#endif

  struct tm timestruct0 = timestruct;
  time_t seconds = SysTimeFromTimeStruct(&timestruct, is_local);

  if (seconds == -1) {
    timestruct = timestruct0;
    timestruct.tm_isdst = 0;
    int64_t seconds_isdst0 = SysTimeFromTimeStruct(&timestruct, is_local);

    timestruct = timestruct0;
    timestruct.tm_isdst = 1;
    int64_t seconds_isdst1 = SysTimeFromTimeStruct(&timestruct, is_local);

    if (seconds_isdst0 < 0)
      seconds = static_cast<time_t>(seconds_isdst1);
    else if (seconds_isdst1 < 0)
      seconds = static_cast<time_t>(seconds_isdst0);
    else
      seconds = static_cast<time_t>(std::min(seconds_isdst0, seconds_isdst1));
  }

  int64_t milliseconds = 0;
  if (seconds == -1 && (exploded.year < 1969 || exploded.year > 1970)) {
    if (exploded.year < 1969) {
      milliseconds = static_cast<int64_t>(std::numeric_limits<int32_t>::min()) *
                     kMillisecondsPerSecond;
    } else {
      milliseconds = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) *
                         kMillisecondsPerSecond +
                     (kMillisecondsPerSecond - 1);
    }
  } else {
    milliseconds = static_cast<int64_t>(seconds) * kMillisecondsPerSecond +
                   exploded.millisecond;
  }

  int64_t microseconds_win_epoch =
      milliseconds * kMicrosecondsPerMillisecond + kTimeTToMicrosecondsOffset;

  DateTime converted_time(microseconds_win_epoch * kTicksPerMicrosecond);

  Exploded to_exploded;
  if (!is_local)
    converted_time.UTCExplode(&to_exploded);
  else
    converted_time.LocalExplode(&to_exploded);

  if (ExplodedMostlyEquals(to_exploded, exploded)) {
    *time = converted_time;
    return true;
  }

  *time = DateTime(0);
  return false;
}

}  // namespace opcua

#endif  // !_WIN32
