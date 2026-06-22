#include "opcua/types/qualified_name.h"

#include "opcua/base/utf_convert.h"

namespace opcua {
std::u16string ToString16(const opcua::QualifiedName& name) {
  return UtfConvert<char16_t>(name.name());
}
}  // namespace opcua
