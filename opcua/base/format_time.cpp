#include "opcua/base/format_time.h"

#include "opcua/base/time/time.h"
#include <format>

std::string FormatTime(opcua::base::Time time, int flags) {
  if (time.is_null())
    return {};

  if (time.is_min())
    return "min";

  if (time.is_max())
    return "max";

  opcua::base::Time::Exploded e = {0};
  if (flags & TIME_FORMAT_UTC)
    time.UTCExplode(&e);
  else
    time.LocalExplode(&e);

  std::string text;

  if (flags & TIME_FORMAT_DATE)
    text = std::format("{:02}.{:02}.{:04}", e.day_of_month, e.month, e.year);

  if (flags & TIME_FORMAT_TIME) {
    std::string tmp =
        std::format("{:02}:{:02}:{:02}", e.hour, e.minute, e.second);
    if (!text.empty())
      text += L' ';
    text += tmp;
    if (flags & TIME_FORMAT_MSEC)
      text += std::format(".{:03}", e.millisecond);
  }

  return text;
}
