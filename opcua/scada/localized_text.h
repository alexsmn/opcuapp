#pragma once

#include <string>
#include <string_view>

#define LOCALIZED_TEXT(text) L##text

namespace opcua::scada {

using LocalizedText = std::u16string;

// Conversion from a opcua::scada::String which is a UTF-8 string.
LocalizedText ToLocalizedText(std::string_view string);

inline LocalizedText ToLocalizedText(const std::u16string_view& string) {
  return LocalizedText{string.data(), string.size()};
}

inline const LocalizedText& ToLocalizedText(const std::u16string& string) {
  return string;
}

inline LocalizedText ToLocalizedText(std::u16string&& string) {
  return string;
}

}  // namespace opcua::scada

// Builds a debug string which is a native MB string. It's not neccessary a
// UTF-8 string like opcua::scada::String.
std::string ToString(const opcua::scada::LocalizedText& text);

inline const std::u16string& ToString16(
    const opcua::scada::LocalizedText& localized_text) {
  return localized_text;
}
