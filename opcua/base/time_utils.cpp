#include "opcua/base/time_utils.h"

#include "opcua/base/format.h"
#include "opcua/base/string_util.h"

#include <cassert>

namespace opcua {
namespace {

#ifndef NDEBUG
opcua::DateTime FloorToMilliseconds(opcua::DateTime time) {
  return opcua::DateTime::FromDeltaSinceWindowsEpoch(
      opcua::Duration::FromMilliseconds(
          time.ToDeltaSinceWindowsEpoch().InMilliseconds()));
}
#endif

}  // namespace

std::string SerializeToString(opcua::Duration delta) {
  int64_t s = delta.InSeconds();
  int64_t m = s / 60;
  s = s % 60;
  int64_t h = m / 60;
  m = m % 60;
  return std::format("{}:{:02}:{:02}", h, m, s);
}

bool Deserialize(std::string_view str, opcua::Duration& delta) {
  auto parts = SplitString(str, ":");
  if (parts.size() != 3)
    return false;

  unsigned h, m, s;
  if (!Parse(parts[0], h) || !Parse(parts[1], m) || !Parse(parts[2], s)) {
    return false;
  }

  delta = opcua::Duration::FromHours(h) +
          opcua::Duration::FromMinutes(m) +
          opcua::Duration::FromSeconds(s);
  return true;
}

std::string SerializeToString(opcua::DateTime time) {
  opcua::DateTime::Exploded e = {0};
  time.UTCExplode(&e);
  auto str = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", e.year, e.month,
                         e.day_of_month, e.hour, e.minute, e.second);

  if (e.millisecond != 0)
    str += std::format(".{:03}", e.millisecond);

#ifndef NDEBUG
  opcua::DateTime parsed_time;
  bool parse_result = Deserialize(str, parsed_time);
  assert(parse_result);
  assert(FloorToMilliseconds(time) == parsed_time);
#endif

  return str;
}

bool Deserialize(std::string_view str, opcua::DateTime& time) {
  return opcua::DateTime::FromUTCString(std::string{str}.c_str(), &time);
}
}  // namespace opcua
