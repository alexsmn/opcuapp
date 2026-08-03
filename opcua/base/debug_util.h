#pragma once

// Umbrella header for the debug/streaming helpers that used to be defined here.
// Prefer including the specific header you need:
//   opcua/base/stream_utf.h      - wide / UTF-16 string stream adapters
//   opcua/base/container_dump.h  - opcua::AsList / AsDict / AsOpt for containers
//   opcua/base/bit_mask_string.h - opcua::BitMaskToString
//
// This header additionally provides ToString / ToString16. Raw std::
// containers, pairs, and optionals are no longer streamable through
// transitional operator<< shims — wrap them in opcua::AsList / AsDict / AsOpt.

#include "opcua/base/bit_mask_string.h"  // IWYU pragma: export
#include "opcua/base/container_dump.h"   // IWYU pragma: export
#include "opcua/base/stream_utf.h"       // IWYU pragma: export
#include "opcua/base/utf_convert.h"      // UtfConvert, used by ToString16 below.

#include <sstream>
#include <string>

namespace opcua {

// --- ToString / ToString16 --------------------------------------------------

template <class T>
inline std::string ToString(const T& v) {
  std::stringstream s;
  s << v;
  return s.str();
}

template <class T>
inline std::u16string ToString16(const T& v) {
  std::stringstream s;
  s << v;
  return UtfConvert<char16_t>(s.str());
}

}  // namespace opcua
