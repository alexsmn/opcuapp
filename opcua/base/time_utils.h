#pragma once

#include "opcua/base/time/time.h"
#include <chrono>

namespace opcua {
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
}  // namespace opcua
