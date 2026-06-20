#include "opcua/scada/qualified_name.h"

#include "opcua/base/utf_convert.h"

std::u16string ToString16(const opcua::scada::QualifiedName& name) {
  return UtfConvert<char16_t>(name.name());
}
