#ifdef _WIN32

#include "opcua/types/date_time.h"

#include "opcua/base/test/scoped_mock_clock_override.h"

#include <windows.h>

#include <cassert>
#include <cstring>

namespace opcua {
namespace {

int64_t FileTimeToDateTimeTicks(const FILETIME& ft) {
  int64_t result;
  static_assert(sizeof(result) == sizeof(ft));
  std::memcpy(&result, &ft, sizeof(result));
  return result;
}

void DateTimeTicksToFileTime(int64_t ticks, FILETIME* ft) {
  assert(ticks >= 0);
  std::memcpy(ft, &ticks, sizeof(*ft));
}

bool SafeConvertToWord(int in, WORD* out) {
  if (in < 0 || in > std::numeric_limits<WORD>::max()) {
    *out = std::numeric_limits<WORD>::max();
    return false;
  }
  *out = static_cast<WORD>(in);
  return true;
}

}  // namespace

DateTime DateTime::FromFileTime(FILETIME ft) {
  return DateTime(FileTimeToDateTimeTicks(ft));
}

FILETIME DateTime::ToFileTime() const {
  FILETIME ft;
  DateTimeTicksToFileTime(ticks_, &ft);
  return ft;
}

DateTime DateTime::Now() {
  if (auto* mock = base::ScopedMockClockOverride::current())
    return mock->Now();
  FILETIME ft;
  ::GetSystemTimePreciseAsFileTime(&ft);
  return DateTime(FileTimeToDateTimeTicks(ft));
}

void DateTime::Explode(bool is_local, Exploded* exploded) const {
  if (ticks_ < 0LL) {
    ZeroMemory(exploded, sizeof(*exploded));
    return;
  }

  FILETIME utc_ft;
  DateTimeTicksToFileTime(ticks_, &utc_ft);

  SYSTEMTIME st = {0};
  bool success = true;
  if (is_local) {
    SYSTEMTIME utc_st;
    success = FileTimeToSystemTime(&utc_ft, &utc_st) &&
              SystemTimeToTzSpecificLocalTime(nullptr, &utc_st, &st);
  } else {
    success = !!FileTimeToSystemTime(&utc_ft, &st);
  }

  if (!success) {
    assert(false && "Unable to convert time");
    ZeroMemory(exploded, sizeof(*exploded));
    return;
  }

  exploded->year = st.wYear;
  exploded->month = st.wMonth;
  exploded->day_of_week = st.wDayOfWeek;
  exploded->day_of_month = st.wDay;
  exploded->hour = st.wHour;
  exploded->minute = st.wMinute;
  exploded->second = st.wSecond;
  exploded->millisecond = st.wMilliseconds;
}

bool DateTime::FromExploded(bool is_local,
                            const Exploded& exploded,
                            DateTime* time) {
  SYSTEMTIME st;
  if (!SafeConvertToWord(exploded.year, &st.wYear) ||
      !SafeConvertToWord(exploded.month, &st.wMonth) ||
      !SafeConvertToWord(exploded.day_of_week, &st.wDayOfWeek) ||
      !SafeConvertToWord(exploded.day_of_month, &st.wDay) ||
      !SafeConvertToWord(exploded.hour, &st.wHour) ||
      !SafeConvertToWord(exploded.minute, &st.wMinute) ||
      !SafeConvertToWord(exploded.second, &st.wSecond) ||
      !SafeConvertToWord(exploded.millisecond, &st.wMilliseconds)) {
    *time = DateTime(0);
    return false;
  }

  FILETIME ft;
  bool success = true;
  if (is_local) {
    SYSTEMTIME utc_st;
    success = TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc_st) &&
              SystemTimeToFileTime(&utc_st, &ft);
  } else {
    success = !!SystemTimeToFileTime(&st, &ft);
  }

  if (!success) {
    *time = DateTime(0);
    return false;
  }

  *time = DateTime(FileTimeToDateTimeTicks(ft));
  return true;
}

}  // namespace opcua

#endif  // _WIN32
