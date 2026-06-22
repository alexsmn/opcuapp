#pragma once

#include <string>
#include <string_view>

namespace opcua {
std::string Format(unsigned char value);
std::u16string WideFormat(unsigned char value);

std::string Format(unsigned short value);
std::u16string WideFormat(unsigned short value);

std::string Format(int value);
std::u16string WideFormat(int value);

std::string Format(unsigned int value);
std::u16string WideFormat(unsigned int value);

std::string Format(long value);
std::u16string WideFormat(long value);

std::string Format(unsigned long value);
std::u16string WideFormat(unsigned long value);

std::string Format(long long value);
std::u16string WideFormat(long long value);

std::string Format(unsigned long long value);
std::u16string WideFormat(unsigned long long value);

std::string Format(float value);
std::u16string WideFormat(float value);

std::string Format(double value);
std::u16string WideFormat(double value);

// String can be implicitly casted to bool.
// std::string Format(bool value);
// std::u16string WideFormat(bool value);

std::string Format(std::string_view value);
std::u16string WideFormat(std::string_view value);

std::string Format(std::u16string_view value);
std::u16string WideFormat(std::u16string_view value);

template <typename T>
bool Parse(const std::string_view& str, T& value);
template <typename T>
bool Parse(const std::u16string_view& str, T& value);

std::string FormatHexBuffer(const void* buf, size_t len);
}  // namespace opcua
