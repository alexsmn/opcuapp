#pragma once

#include <string>
#include <string_view>

#include <boost/locale/encoding_utf.hpp>

// UtfConvert: convert between UTF encodings (UTF-8, UTF-16, wide).
// Wraps boost::locale::conv::utf_to_utf with string_view support.

template <typename CharOut, typename CharIn>
std::basic_string<CharOut> UtfConvert(std::basic_string_view<CharIn> sv) {
  return boost::locale::conv::utf_to_utf<CharOut>(sv.data(),
                                                   sv.data() + sv.size());
}

template <typename CharOut, typename CharIn>
std::basic_string<CharOut> UtfConvert(const std::basic_string<CharIn>& str) {
  return boost::locale::conv::utf_to_utf<CharOut>(str);
}

template <typename CharOut, typename CharIn>
std::basic_string<CharOut> UtfConvert(const CharIn* str) {
  return boost::locale::conv::utf_to_utf<CharOut>(str);
}
