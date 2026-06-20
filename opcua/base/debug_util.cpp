#include "opcua/base/debug_util.h"

#include "opcua/base/utf_convert.h"

std::ostream& operator<<(std::ostream& stream, const std::wstring& v) {
  return stream << UtfConvert<char>(v);
}

std::string BitMaskToString(unsigned bit_mask,
                            std::span<const std::string_view> bit_strings) {
  std::string result = "[";
  bool first = true;
  for (std::size_t i = 0; i < bit_strings.size(); ++i) {
    if (bit_mask & (1 << i)) {
      if (!first)
        result += ",";
      result += '"';
      result += bit_strings[i];
      result += '"';
      first = false;
    }
  }
  result += "]";
  return result;
}
