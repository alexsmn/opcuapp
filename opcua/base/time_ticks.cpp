#include "opcua/base/time_ticks.h"

#include <ostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace opcua {
namespace base {

std::ostream& operator<<(std::ostream& os, TimeTicks time_ticks) {
  const Duration as_duration = time_ticks - TimeTicks();
  return os << as_duration.InMicroseconds() << " bogo-microseconds";
}

#ifdef _WIN32

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
    return TimeTicks(now.QuadPart * Duration::kMicrosecondsPerSecond / freq);
  }
  int64_t whole_seconds = now.QuadPart / freq;
  int64_t leftover = now.QuadPart - (whole_seconds * freq);
  return TimeTicks(whole_seconds * Duration::kMicrosecondsPerSecond +
                   leftover * Duration::kMicrosecondsPerSecond / freq);
}

#else

namespace {

int64_t ConvertTimespecToMicros(const struct timespec& ts) {
  int64_t result = ts.tv_sec;
  result *= Duration::kMicrosecondsPerSecond;
  result += (ts.tv_nsec / Duration::kNanosecondsPerMicrosecond);
  return result;
}

}  // namespace

TimeTicks TimeTicks::Now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return TimeTicks(ConvertTimespecToMicros(ts));
}

#endif  // _WIN32

}  // namespace base
}  // namespace opcua
