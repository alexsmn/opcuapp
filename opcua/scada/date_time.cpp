#include "opcua/scada/date_time.h"

#include "opcua/base/format_time.h"
#include "opcua/base/utf_convert.h"

namespace opcua {
std::string ToString(opcua::DateTime time) {
  return FormatTime(time);
}

std::u16string ToString16(opcua::DateTime time) {
  return UtfConvert<char16_t>(FormatTime(time));
}

/*std::ostream& operator<<(std::ostream& stream, opcua::DateTime time) {
  return stream << FormatTime(time);
}*/
}  // namespace opcua (vendored)
