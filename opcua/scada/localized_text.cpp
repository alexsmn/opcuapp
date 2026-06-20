#include "opcua/scada/localized_text.h"

#include "opcua/base/utf_convert.h"

namespace opcua::scada {

LocalizedText ToLocalizedText(std::string_view string) {
  return UtfConvert<char16_t>(string);
}

}  // namespace opcua::scada

std::string ToString(const opcua::scada::LocalizedText& text) {
  // E.g. see `AuthenticationTask` on how login user name is translated to the
  // qualified name.
  return UtfConvert<char>(text);
}
