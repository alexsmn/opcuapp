#include "opcua/scada/date_time.h"

#include "opcua/base/format_time.h"
#include "opcua/base/utf_convert.h"

std::string ToString(opcua::scada::DateTime time) {
  return FormatTime(time);
}

std::u16string ToString16(opcua::scada::DateTime time) {
  return UtfConvert<char16_t>(FormatTime(time));
}

/*std::ostream& operator<<(std::ostream& stream, opcua::scada::DateTime time) {
  return stream << FormatTime(time);
}*/
