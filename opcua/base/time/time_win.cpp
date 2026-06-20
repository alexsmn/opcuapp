#ifdef _WIN32

#include "opcua/base/time/time.h"

#include "opcua/base/test/scoped_mock_clock_override.h"

#include <windows.h>

#include <cassert>
#include <cstring>

namespace opcua {
namespace {

int64_t FileTimeToMicroseconds(const FILETIME& ft) {
  int64_t result;
  static_assert(sizeof(result) == sizeof(ft));
  std::memcpy(&result, &ft, sizeof(result));
  return result / 10;  // 100-nanoseconds to microseconds.
}

void MicrosecondsToFileTime(int64_t us, FILETIME* ft) {
  assert(us >= 0);
  int64_t val = us * 10;
  std::memcpy(ft, &val, sizeof(*ft));
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

namespace base {

// Time -----------------------------------------------------------------------

Time Time::FromFileTime(FILETIME ft) {
  return Time(FileTimeToMicroseconds(ft));
}

FILETIME Time::ToFileTime() const {
  FILETIME ft;
  MicrosecondsToFileTime(us_, &ft);
  return ft;
}

Time Time::Now() {
  if (auto* mock = ScopedMockClockOverride::current())
    return mock->Now();
  FILETIME ft;
  ::GetSystemTimePreciseAsFileTime(&ft);
  return Time(FileTimeToMicroseconds(ft));
}

void Time::Explode(bool is_local, Exploded* exploded) const {
  if (us_ < 0LL) {
    ZeroMemory(exploded, sizeof(*exploded));
    return;
  }

  FILETIME utc_ft;
  MicrosecondsToFileTime(us_, &utc_ft);

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

bool Time::FromExploded(bool is_local, const Exploded& exploded, Time* time) {
  SYSTEMTIME st;
  if (!SafeConvertToWord(exploded.year, &st.wYear) ||
      !SafeConvertToWord(exploded.month, &st.wMonth) ||
      !SafeConvertToWord(exploded.day_of_week, &st.wDayOfWeek) ||
      !SafeConvertToWord(exploded.day_of_month, &st.wDay) ||
      !SafeConvertToWord(exploded.hour, &st.wHour) ||
      !SafeConvertToWord(exploded.minute, &st.wMinute) ||
      !SafeConvertToWord(exploded.second, &st.wSecond) ||
      !SafeConvertToWord(exploded.millisecond, &st.wMilliseconds)) {
    *time = Time(0);
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
    *time = Time(0);
    return false;
  }

  *time = Time(FileTimeToMicroseconds(ft));
  return true;
}

// TimeTicks ------------------------------------------------------------------

namespace {

int64_t QPCFrequency() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return f;
  }();
  return freq.QuadPart;
}

}  // namespace

TimeTicks TimeTicks::Now() {
  LARGE_INTEGER now;
  ::QueryPerformanceCounter(&now);
  int64_t freq = QPCFrequency();
  // Avoid overflow: if count is small, multiply first; otherwise divide first.
  constexpr int64_t kOverflowThreshold = INT64_C(0x8637BD05AF7);
  if (now.QuadPart < kOverflowThreshold) {
    return TimeTicks(now.QuadPart * Time::kMicrosecondsPerSecond / freq);
  }
  int64_t whole_seconds = now.QuadPart / freq;
  int64_t leftover = now.QuadPart - (whole_seconds * freq);
  return TimeTicks(whole_seconds * Time::kMicrosecondsPerSecond +
                   leftover * Time::kMicrosecondsPerSecond / freq);
}

}  // namespace base

#endif  // _WIN32
}  // namespace opcua (vendored)
