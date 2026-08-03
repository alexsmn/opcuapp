#include "opcua/base/format.h"

#include "opcua/base/u16format.h"
#include "opcua/base/utf_convert.h"

#include <cassert>
#include <charconv>
#include <format>

namespace opcua {
std::string Format(double value) {
  char buffer[32];
  auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                 std::chars_format::general);
  return std::string(buffer, ptr);
}

std::u16string WideFormat(double value) {
  char buffer[32];
  auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                 std::chars_format::general);
  return std::u16string(buffer, ptr);
}

std::string Format(float value) {
  return Format(static_cast<double>(value));
}

std::u16string WideFormat(float value) {
  return WideFormat(static_cast<double>(value));
}

std::string Format(int value) {
  return std::to_string(value);
}

std::u16string WideFormat(int value) {
  return u16format(L"{}", value);
}

std::string Format(unsigned int value) {
  return std::to_string(value);
}

std::u16string WideFormat(unsigned int value) {
  return u16format(L"{}", value);
}

std::string Format(long value) {
  return std::to_string(value);
}

std::u16string WideFormat(long value) {
  return u16format(L"{}", value);
}

std::string Format(unsigned long value) {
  return std::to_string(value);
}

std::u16string WideFormat(unsigned long value) {
  return u16format(L"{}", value);
}

std::string Format(long long value) {
  return std::to_string(value);
}

std::u16string WideFormat(long long value) {
  return u16format(L"{}", value);
}

std::string Format(unsigned long long value) {
  return std::to_string(value);
}

std::u16string WideFormat(unsigned long long value) {
  return u16format(L"{}", value);
}

// String can be implicitly casted to bool.
/*std::string Format(bool value) {
  return Format(value ? 1 : 0);
}

std::u16string WideFormat(bool value) {
  return WideFormat(value ? 1 : 0);
}*/

std::string Format(unsigned char value) {
  return Format(static_cast<unsigned>(value));
}

std::u16string WideFormat(unsigned char value) {
  return WideFormat(static_cast<unsigned>(value));
}

std::string Format(unsigned short value) {
  return Format(static_cast<unsigned>(value));
}

std::u16string WideFormat(unsigned short value) {
  return WideFormat(static_cast<unsigned>(value));
}

std::string Format(std::string_view value) {
  return std::string{value};
}

std::u16string WideFormat(std::string_view value) {
  return UtfConvert<char16_t>(value);
}

std::string Format(std::u16string_view value) {
  return UtfConvert<char>(value);
}

std::u16string WideFormat(std::u16string_view value) {
  return std::u16string{value};
}

namespace {

template <typename T>
bool FromChars(std::string_view str, T& value) {
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc() && ptr == str.data() + str.size();
}

template <typename T>
bool FromChars16(std::u16string_view str, T& value) {
  auto narrow = UtfConvert<char>(str);
  return FromChars(std::string_view{narrow}, value);
}

}  // namespace

template <>
bool Parse(const std::string_view& str, int32_t& value) {
  return FromChars(str, value);
}

template <>
bool Parse(const std::u16string_view& str, int32_t& value) {
  return FromChars16(str, value);
}

template <>
bool Parse(const std::string_view& str, uint32_t& value) {
  return FromChars(str, value);
}

template <>
bool Parse(const std::u16string_view& str, uint32_t& value) {
  return FromChars16(str, value);
}

template <>
bool Parse(const std::string_view& str, int8_t& value) {
  int tmp;
  if (!Parse(str, tmp))
    return false;
  value = static_cast<int8_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::u16string_view& str, int8_t& value) {
  int tmp;
  if (!Parse(str, tmp))
    return false;
  value = static_cast<int8_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::string_view& str, uint8_t& value) {
  unsigned tmp;
  if (!Parse(str, tmp))
    return false;
  value = static_cast<uint8_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::u16string_view& str, uint8_t& value) {
  unsigned tmp;
  if (!Parse(str, tmp))
    return false;
  value = static_cast<uint8_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::string_view& str, int16_t& value) {
  int tmp;
  if (!FromChars(str, tmp))
    return false;
  value = static_cast<int16_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::u16string_view& str, int16_t& value) {
  int tmp;
  if (!FromChars16(str, tmp))
    return false;
  value = static_cast<int16_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::string_view& str, uint16_t& value) {
  unsigned tmp;
  if (!FromChars(str, tmp))
    return false;
  value = static_cast<uint16_t>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::u16string_view& str, uint16_t& value) {
  unsigned tmp;
  if (!FromChars16(str, tmp))
    return false;
  value = static_cast<unsigned short>(tmp);
  return value == tmp;
}

template <>
bool Parse(const std::string_view& str, int64_t& value) {
  return FromChars(str, value);
}

template <>
bool Parse(const std::u16string_view& str, int64_t& value) {
  return FromChars16(str, value);
}

template <>
bool Parse(const std::string_view& str, uint64_t& value) {
  return FromChars(str, value);
}

template <>
bool Parse(const std::u16string_view& str, uint64_t& value) {
  return FromChars16(str, value);
}

template <>
bool Parse(const std::string_view& str, double& value) {
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc() && ptr == str.data() + str.size();
}
template <>
bool Parse(const std::u16string_view& str, double& value) {
  auto narrow = UtfConvert<char>(str);
  return Parse(std::string_view{narrow}, value);
}

template <>
bool Parse(const std::string_view& str, bool& value) {
  int tmp;
  if (!Parse(str, tmp))
    return false;
  value = tmp != 0;
  return true;
}

template <>
bool Parse(const std::u16string_view& str, bool& value) {
  int tmp;
  if (!Parse(str, tmp))
    return false;
  value = tmp != 0;
  return true;
}

template <>
bool Parse(const std::string_view& str, std::string& value) {
  value = std::string{str};
  return true;
}
template <>
bool Parse(const std::u16string_view& str, std::u16string& value) {
  value = std::u16string{str};
  return true;
}

std::string FormatHexBuffer(const void* buf, size_t len) {
  assert(buf);
  if (!len)
    return std::string();

  std::string result;
  result.reserve(len * 2);
  const auto* bytes = static_cast<const uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i)
    result += std::format("{:02X}", bytes[i]);
  return result;
}
}  // namespace opcua
