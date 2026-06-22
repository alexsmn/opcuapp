#include "opcua/types/localized_text.h"

#include "opcua/base/utf_convert.h"

namespace opcua {

LocalizedText ToLocalizedText(std::string_view string) {
  return UtfConvert<char16_t>(string);
}

std::string ToString(const opcua::LocalizedText& text) {
  // E.g. see `AuthenticationTask` on how login user name is translated to the
  // qualified name.
  return UtfConvert<char>(text);
}
}  // namespace opcua
