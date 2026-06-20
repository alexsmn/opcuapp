#pragma once

#include "opcua/base/time/time.h"
#include <chrono>

namespace opcua {
inline opcua::base::TimeDelta TimeDeltaFromSecondsF(double dt) {
  return opcua::base::TimeDelta::FromMicroseconds(static_cast<int64_t>(
      dt * static_cast<double>(opcua::base::Time::kMicrosecondsPerSecond)));
}

std::string SerializeToString(opcua::base::TimeDelta delta);
bool Deserialize(std::string_view str, opcua::base::TimeDelta& delta);

std::string SerializeToString(opcua::base::Time time);
bool Deserialize(std::string_view str, opcua::base::Time& time);

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
inline auto AsChrono(opcua::base::TimeDelta delta) {
  return std::chrono::duration_cast<T>(
      std::chrono::nanoseconds{delta.InNanoseconds()});
}

template <>
inline auto AsChrono<std::chrono::nanoseconds>(opcua::base::TimeDelta delta) {
  return std::chrono::nanoseconds{delta.InNanoseconds()};
}

template <>
inline auto AsChrono<std::chrono::milliseconds>(opcua::base::TimeDelta delta) {
  return std::chrono::milliseconds{delta.InMilliseconds()};
}

template <typename T = std::chrono::system_clock::time_point>
inline auto AsChrono(opcua::base::Time time) {
  return T{AsChrono<typename T::duration>(time - opcua::base::Time::UnixEpoch())};
}

// Example of truncation to a second:
//   TruncateTimeTo(opcua::base::Time::Now(), opcua::base::TimeDelta::FromSeconds(1));
inline opcua::base::Time TruncateTimeTo(opcua::base::Time time, opcua::base::TimeDelta interval) {
  auto delta = time - opcua::base::Time::UnixEpoch();
  auto remainder = opcua::base::TimeDelta::FromNanoseconds(delta.InNanoseconds() %
                                                    interval.InNanoseconds());
  return time - remainder;
}
}  // namespace opcua (vendored)
