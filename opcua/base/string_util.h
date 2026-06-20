#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

// SplitString: split by delimiter substring (not character set).
// boost::split is not useful for delimiter string.
std::vector<std::string_view> SplitString(std::string_view str,
                                          std::string_view delimiter);

inline std::vector<std::string_view> SplitString(std::string_view str,
                                                 char delimiter) {
  return SplitString(str, std::string_view{&delimiter, 1});
}

// JoinStrings: join string_views with a delimiter.
// boost::algorithm::join does not support std::string_view.
std::string JoinStrings(std::span<const std::string_view> strings,
                        std::string_view delimiter);

// IsStringASCII: returns true if all characters are in the ASCII range
// (0x00-0x7F). Executes in time determined solely by string length, not
// contents (timing-safe).
bool IsStringASCII(std::string_view str);

template <class Char>
constexpr Char ToLowerAscii(Char ch) {
  return ch >= static_cast<Char>('A') && ch <= static_cast<Char>('Z')
      ? static_cast<Char>(ch - static_cast<Char>('A') + static_cast<Char>('a'))
      : ch;
}

bool IEqualsAscii(std::string_view left, std::string_view right);
bool IEqualsAscii(std::u16string_view left, std::u16string_view right);

void TrimAsciiWhitespace(std::string& str);
void TrimAsciiWhitespace(std::u16string& str);

inline bool IsAsciiCaseInsensitiveEqual(std::u16string_view left,
                                        std::u16string_view right) {
  return IEqualsAscii(left, right);
}

// ReplaceSubstringsAfterOffset: starting at |start_offset|, replace all
// instances of |find_this| with |replace_with|. Modifies |str| in place.
void ReplaceSubstringsAfterOffset(std::u16string* str,
                                  size_t start_offset,
                                  std::u16string_view find_this,
                                  std::u16string_view replace_with);

void ReplaceSubstringsAfterOffset(std::string* str,
                                  size_t start_offset,
                                  std::string_view find_this,
                                  std::string_view replace_with);

// ReplaceFirstSubstringAfterOffset: starting at |start_offset|, replace the
// first instance of |find_this| with |replace_with|. Modifies |str| in place.
void ReplaceFirstSubstringAfterOffset(std::string* str,
                                      size_t start_offset,
                                      std::string_view find_this,
                                      std::string_view replace_with);

// WriteInto: resizes |str| and returns a writable pointer for C APIs.
template <typename StringType>
typename StringType::value_type* WriteInto(StringType* str,
                                           size_t length_with_null) {
  str->resize(length_with_null - 1);
  return str->data();
}
