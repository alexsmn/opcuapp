#pragma once

#include "opcua/types/date_time.h"
#include <chrono>

namespace opcua {
std::string SerializeToString(opcua::Duration delta);
bool Deserialize(std::string_view str, opcua::Duration& delta);

std::string SerializeToString(opcua::DateTime time);
bool Deserialize(std::string_view str, opcua::DateTime& time);

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
