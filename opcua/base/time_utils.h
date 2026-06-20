#pragma once

#include "opcua/base/time/time.h"
#include <chrono>

namespace opcua {
inline base::TimeDelta TimeDeltaFromSecondsF(double dt) {
  return base::TimeDelta::FromMicroseconds(static_cast<int64_t>(
      dt * static_cast<double>(base::Time::kMicrosecondsPerSecond)));
}

std::string SerializeToString(base::TimeDelta delta);
bool Deserialize(std::string_view str, base::TimeDelta& delta);

std::string SerializeToString(base::Time time);
bool Deserialize(std::string_view str, base::Time& time);

template <class Rep, class Period>
inline auto InMilliseconds(const std::chrono::duration<Rep, Period>& duration) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
      .count();
}

template <class Rep, class Period>
inline auto InSeconds(const std::chrono::duration<Rep, Period>& duration) {
  return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
}

template <class T>
inline auto AsChrono(base::TimeDelta delta) {
  return std::chrono::duration_cast<T>(
      std::chrono::nanoseconds{delta.InNanoseconds()});
}

template <>
inline auto AsChrono<std::chrono::nanoseconds>(base::TimeDelta delta) {
  return std::chrono::nanoseconds{delta.InNanoseconds()};
}

template <>
inline auto AsChrono<std::chrono::milliseconds>(base::TimeDelta delta) {
  return std::chrono::milliseconds{delta.InMilliseconds()};
}

template <typename T = std::chrono::system_clock::time_point>
inline auto AsChrono(base::Time time) {
  return T{AsChrono<typename T::duration>(time - base::Time::UnixEpoch())};
}

// Example of truncation to a second:
//   TruncateTimeTo(base::Time::Now(), base::TimeDelta::FromSeconds(1));
inline base::Time TruncateTimeTo(base::Time time, base::TimeDelta interval) {
  auto delta = time - base::Time::UnixEpoch();
  auto remainder = base::TimeDelta::FromNanoseconds(delta.InNanoseconds() %
                                                    interval.InNanoseconds());
  return time - remainder;
}
}  // namespace opcua (vendored)
