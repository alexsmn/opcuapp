#pragma once

#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

// std::format for char16_t strings (not natively supported by std::format).
// On Windows, wchar_t and char16_t are both 16-bit, so we format as wstring
// and reinterpret.

namespace opcua {
namespace u16format_detail {

inline std::wstring ToWide(std::u16string_view value) {
  std::wstring result;
  result.reserve(value.size());
  for (const char16_t ch : value) {
    result.push_back(static_cast<wchar_t>(ch));
  }
  return result;
}

inline std::u16string FromWide(std::wstring_view value) {
  std::u16string result;
  result.reserve(value.size());
  for (const wchar_t ch : value) {
    result.push_back(static_cast<char16_t>(ch));
  }
  return result;
}

// Convert args to wchar_t-compatible types for std::format:
// - u16string/u16string_view/char16_t* -> wstring
// - std::string -> std::wstring (widen)
// - Scoped enums/plain enums -> underlying type
// - Everything else -> pass through
template <typename T>
auto to_wide_arg(T&& arg) {
  using D = std::decay_t<T>;
  if constexpr (std::is_same_v<D, std::u16string>) {
    return ToWide(arg);
  } else if constexpr (std::is_same_v<D, std::u16string_view>) {
    return ToWide(arg);
  } else if constexpr (std::is_same_v<D, const char16_t*> ||
                        std::is_same_v<D, char16_t*>) {
    return ToWide(std::u16string_view{arg});
  } else if constexpr (std::is_same_v<D, std::string>) {
    return std::wstring(arg.begin(), arg.end());
  } else if constexpr (std::is_same_v<D, std::string_view>) {
    return std::wstring(arg.begin(), arg.end());
  } else if constexpr (std::is_enum_v<D>) {
    return static_cast<std::underlying_type_t<D>>(arg);
  } else {
    return std::forward<T>(arg);
  }
}

}  // namespace u16format_detail

template <typename... Args>
std::u16string u16format(std::wstring_view fmt, Args&&... args) {
  // Store converted args in a tuple so they are lvalues for make_wformat_args.
  auto converted = std::make_tuple(
      u16format_detail::to_wide_arg(std::forward<Args>(args))...);
  return std::apply(
      [&](auto&... wide_args) {
        std::wstring wide =
            std::vformat(fmt, std::make_wformat_args(wide_args...));
        return u16format_detail::FromWide(wide);
      },
      converted);
}
}  // namespace opcua (vendored)
