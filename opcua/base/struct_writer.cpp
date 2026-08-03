#include "opcua/base/struct_writer.h"

#include "opcua/base/utf_convert.h"

namespace opcua {
template <>
void StructWriter::AddValue(const std::wstring& value) {
  stream_ << "\"" << UtfConvert<char>(value) << "\"";
}

template <>
void StructWriter::AddValue(const std::u16string& value) {
  stream_ << "\"" << UtfConvert<char>(value) << "\"";
}
}  // namespace opcua
