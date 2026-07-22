#pragma once

#include "opcua/types/string.h"

#include <ostream>

namespace opcua {

// Built-in OPC UA XmlElement: a serialized XML fragment, carried as its UTF-8
// text. It is a distinct type rather than an alias for String because a Variant
// must be able to tell the two apart — they have different BuiltInType ids and
// different Binary encodings (XmlElement is length-prefixed as a ByteString).
// OPC UA Part 3 §8.38 XmlElement,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.38
struct XmlElement {
  String value;

  bool empty() const { return value.empty(); }

  bool operator==(const XmlElement& other) const = default;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const XmlElement& element) {
  return stream << '"' << element.value << '"';
}

inline const String& ToString(const XmlElement& element) {
  return element.value;
}

}  // namespace opcua
