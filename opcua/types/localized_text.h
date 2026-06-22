#pragma once

#include <string>
#include <string_view>

#define LOCALIZED_TEXT(text) L##text

namespace opcua {

// Built-in OPC UA LocalizedText: human-readable text with an associated locale;
// here only the text is retained, as a UTF-16 string. OPC UA Part 3 §8.5
// LocalizedText, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.5
using LocalizedText = std::u16string;

// Conversion from a opcua::String which is a UTF-8 string.
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

// Builds a debug string which is a native MB string. It's not neccessary a
// UTF-8 string like opcua::String.
std::string ToString(const opcua::LocalizedText& text);

inline const std::u16string& ToString16(
    const opcua::LocalizedText& localized_text) {
  return localized_text;
}
}  // namespace opcua
