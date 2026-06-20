#include "opcua/base/string_util.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>

// SplitString: split by delimiter substring.
std::vector<std::string_view> SplitString(std::string_view str,
                                          std::string_view delimiter) {
  if (str.empty())
    return {};

  std::vector<std::string_view> parts;

  for (;;) {
    auto p = str.find(delimiter);
    if (p == delimiter.npos) {
      parts.push_back(str);
      break;
    }

    parts.push_back(str.substr(0, p));
    str = str.substr(p + delimiter.size());
  }

  return parts;
}

// JoinStrings: join string_views with a delimiter.
std::string JoinStrings(std::span<const std::string_view> strings,
                        std::string_view delimiter) {
  if (strings.empty())
    return {};

  const auto strings_combined_size = std::accumulate(
      strings.begin(), strings.end(), static_cast<size_t>(0),
      [](std::size_t sum, std::string_view str) { return sum + str.size(); });
  const auto delimiters_size = delimiter.size() * (strings.size() - 1);
  const auto result_size = strings_combined_size + delimiters_size;

  std::string result;
  result.reserve(result_size);

  result.append(strings[0].data(), strings[0].size());
  for (size_t i = 1; i < strings.size(); ++i) {
    result.append(delimiter.data(), delimiter.size());
    result.append(strings[i].data(), strings[i].size());
  }

  assert(result.size() == result_size);
  return result;
}

// IsStringASCII: timing-safe ASCII check.
// Adapted from Chromium base/strings/string_util_internal.h.
namespace {

using MachineWord = uintptr_t;

inline bool IsMachineWordAligned(const void* pointer) {
  return !(reinterpret_cast<MachineWord>(pointer) & (sizeof(MachineWord) - 1));
}

template <class Char>
bool DoIsStringASCII(const Char* characters, size_t length) {
  constexpr MachineWord NonASCIIMasks[] = {
      0,
      static_cast<MachineWord>(0x8080808080808080ULL),
      static_cast<MachineWord>(0xFF80FF80FF80FF80ULL),
      0,
      static_cast<MachineWord>(0xFFFFFF80FFFFFF80ULL),
  };

  if (!length)
    return true;
  constexpr MachineWord non_ascii_bit_mask = NonASCIIMasks[sizeof(Char)];
  static_assert(non_ascii_bit_mask, "Error: Invalid Mask");
  MachineWord all_char_bits = 0;
  const Char* end = characters + length;

  // Prologue: align the input.
  while (!IsMachineWordAligned(characters) && characters < end)
    all_char_bits |= *characters++;
  if (all_char_bits & non_ascii_bit_mask)
    return false;

  // Compare the values of CPU word size.
  constexpr size_t chars_per_word = sizeof(MachineWord) / sizeof(Char);
  constexpr int batch_count = 16;
  while (characters <= end - batch_count * chars_per_word) {
    all_char_bits = 0;
    for (int i = 0; i < batch_count; ++i) {
      all_char_bits |= *(reinterpret_cast<const MachineWord*>(characters));
      characters += chars_per_word;
    }
    if (all_char_bits & non_ascii_bit_mask)
      return false;
  }

  // Process the remaining words.
  all_char_bits = 0;
  while (characters <= end - chars_per_word) {
    all_char_bits |= *(reinterpret_cast<const MachineWord*>(characters));
    characters += chars_per_word;
  }

  // Process the remaining bytes.
  while (characters < end)
    all_char_bits |= *characters++;

  return !(all_char_bits & non_ascii_bit_mask);
}

}  // namespace

bool IsStringASCII(std::string_view str) {
  return DoIsStringASCII(str.data(), str.length());
}

template <class StringView>
bool IEqualsAsciiT(StringView left, StringView right) {
  if (left.size() != right.size())
    return false;

  for (size_t i = 0; i < left.size(); ++i) {
    if (ToLowerAscii(left[i]) != ToLowerAscii(right[i]))
      return false;
  }

  return true;
}

bool IEqualsAscii(std::string_view left, std::string_view right) {
  return IEqualsAsciiT(left, right);
}

bool IEqualsAscii(std::u16string_view left, std::u16string_view right) {
  return IEqualsAsciiT(left, right);
}

template <class String>
void TrimAsciiWhitespaceT(String& str) {
  auto is_space = [](auto ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };
  str.erase(str.begin(), std::find_if_not(str.begin(), str.end(), is_space));
  str.erase(std::find_if_not(str.rbegin(), str.rend(), is_space).base(),
            str.end());
}

void TrimAsciiWhitespace(std::string& str) {
  TrimAsciiWhitespaceT(str);
}

void TrimAsciiWhitespace(std::u16string& str) {
  TrimAsciiWhitespaceT(str);
}

// ReplaceSubstringsAfterOffset implementations.
void ReplaceSubstringsAfterOffset(std::u16string* str,
                                  size_t start_offset,
                                  std::u16string_view find_this,
                                  std::u16string_view replace_with) {
  if (find_this.empty())
    return;
  size_t pos = start_offset;
  while ((pos = str->find(find_this, pos)) != std::u16string::npos) {
    str->replace(pos, find_this.size(), replace_with);
    pos += replace_with.size();
  }
}

void ReplaceSubstringsAfterOffset(std::string* str,
                                  size_t start_offset,
                                  std::string_view find_this,
                                  std::string_view replace_with) {
  if (find_this.empty())
    return;
  size_t pos = start_offset;
  while ((pos = str->find(find_this, pos)) != std::string::npos) {
    str->replace(pos, find_this.size(), replace_with);
    pos += replace_with.size();
  }
}

void ReplaceFirstSubstringAfterOffset(std::string* str,
                                      size_t start_offset,
                                      std::string_view find_this,
                                      std::string_view replace_with) {
  if (find_this.empty())
    return;
  size_t pos = str->find(find_this, start_offset);
  if (pos != std::string::npos)
    str->replace(pos, find_this.size(), replace_with);
}
