#pragma once

// UTF stream adapters (opcua namespace): stream wide (std::wstring /
// std::wstring_view) and UTF-16 (std::u16string / std::u16string_view) strings
// to a char-based stream by transcoding to UTF-8 first.
//
// The wide-string operators are templated on the stream type; the return type
// is the insertion expression's own type so a derived stream such as
// std::ostringstream (whose `<< std::string_view` yields std::ostream&) binds
// correctly. The u16string operators are std::ostream-fixed; the
// boost::log::formatting_ostream counterparts live in opcua/base/boost_log.h
// next to the Boost.Log include they require.

#include "opcua/base/utf_convert.h"

#include <ostream>
#include <string>
#include <string_view>

namespace opcua {

inline std::ostream& operator<<(std::ostream& stream, const std::u16string& s) {
  return stream << UtfConvert<char>(s);
}

inline std::ostream& operator<<(std::ostream& stream, std::u16string_view s) {
  return stream << UtfConvert<char>(s);
}

template <class StreamT>
inline auto operator<<(StreamT& stream, const std::wstring& s)
    -> decltype(stream << std::string_view{}) {
  return stream << UtfConvert<char>(s);
}

template <class StreamT>
inline auto operator<<(StreamT& stream, std::wstring_view s)
    -> decltype(stream << std::string_view{}) {
  return stream << UtfConvert<char>(s);
}

}  // namespace opcua
