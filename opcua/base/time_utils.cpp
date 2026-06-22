#include "opcua/base/time_utils.h"

#include "opcua/base/format.h"
#include "opcua/base/string_util.h"

namespace opcua {
namespace {

#ifndef NDEBUG
opcua::base::Time FloorToMilliseconds(opcua::base::Time time) {
  return opcua::base::Time::FromDeltaSinceWindowsEpoch(
      opcua::base::TimeDelta::FromMilliseconds(
          time.ToDeltaSinceWindowsEpoch().InMilliseconds()));
}
#endif

}  // namespace

std::string SerializeToString(opcua::base::TimeDelta delta) {
  int64_t s = delta.InSeconds();
  int64_t m = s / 60;
  s = s % 60;
  int64_t h = m / 60;
  m = m % 60;
  return std::format("{}:{:02}:{:02}", h, m, s);
}

bool Deserialize(std::string_view str, opcua::base::TimeDelta& delta) {
  auto parts = SplitString(str, ":");
  if (parts.size() != 3)
    return false;

  unsigned h, m, s;
  if (!Parse(parts[0], h) || !Parse(parts[1], m) || !Parse(parts[2], s)) {
    return false;
  }

  delta = opcua::base::TimeDelta::FromHours(h) +
          opcua::base::TimeDelta::FromMinutes(m) +
          opcua::base::TimeDelta::FromSeconds(s);
  return true;
}

std::string SerializeToString(opcua::base::Time time) {
  opcua::base::Time::Exploded e = {0};
  time.UTCExplode(&e);
  auto str = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", e.year, e.month,
                         e.day_of_month, e.hour, e.minute, e.second);

  if (e.millisecond != 0)
    str += std::format(".{:03}", e.millisecond);

#ifndef NDEBUG
  opcua::base::Time parsed_time;
  bool parse_result = Deserialize(str, parsed_time);
  assert(parse_result);
  assert(FloorToMilliseconds(time) == parsed_time);
#endif

  return str;
}

bool Deserialize(std::string_view str, opcua::base::Time& time) {
  return opcua::base::Time::FromUTCString(std::string{str}.c_str(), &time);
}
}  // namespace opcua
