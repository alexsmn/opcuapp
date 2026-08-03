#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/expanded_node_id.h"

#include <any>
#include <ostream>
#include <utility>

namespace opcua {

// Built-in OPC UA ExtensionObject: a container that carries a structured
// (non-built-in) DataType value together with the NodeId of its encoding. OPC
// UA Part 6 §5.1.8 ExtensionObject,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.8
//
// The body is held in whatever form the encoding it arrived in produces — a
// ByteString for the Binary encoding, a boost::json::value for the JSON one —
// so it is stored type-erased. Turning it into a concrete type is the
// consumer's business: see the BinaryEncodingId trait and the
// ToExtensionObject/FromExtensionObject helpers in opcua/ua/ua_binary_codec.h,
// which are generated from the schema. An ExtensionObject whose type id this
// stack does not know keeps its body verbatim and round-trips unchanged.
class ExtensionObject {
 public:
  ExtensionObject() = default;

  ExtensionObject(ExpandedNodeId data_type_id, std::any value)
      : data_type_id_{std::move(data_type_id)}, value_{std::move(value)} {}

  ExtensionObject(const ExtensionObject&) = default;
  ExtensionObject& operator=(const ExtensionObject&) = default;

  ExtensionObject(ExtensionObject&& source) noexcept
      : data_type_id_{std::move(source.data_type_id_)},
        value_{std::move(source.value_)} {}

  ExtensionObject& operator=(ExtensionObject&& source) noexcept {
    if (&value_ != &source.value_) {
      data_type_id_ = std::move(source.data_type_id_);
      value_ = std::move(source.value_);
    }
    return *this;
  }

  const ExpandedNodeId& data_type_id() const { return data_type_id_; }

  std::any& value() { return value_; }
  const std::any& value() const { return value_; }

  bool has_value() const { return value_.has_value(); }

  // The body as raw encoded bytes, or nullptr when it is absent or held in
  // another encoding's representation. Saves callers on the binary path from
  // repeating the any_cast.
  const ByteString* binary_body() const {
    return std::any_cast<ByteString>(&value_);
  }

  // Compares the type id and the body. Bodies in a representation this class
  // cannot inspect (anything but a ByteString) are reported unequal unless
  // both are absent — a conservative answer, since claiming equality for
  // values we cannot compare would be worse. Before 2026-07 this returned a
  // hardcoded `false`, which quietly made every containing type unequal to
  // itself.
  bool operator==(const ExtensionObject& other) const {
    if (data_type_id_ != other.data_type_id_)
      return false;
    if (!value_.has_value() || !other.value_.has_value())
      return value_.has_value() == other.value_.has_value();
    const ByteString* body = binary_body();
    const ByteString* other_body = other.binary_body();
    return body != nullptr && other_body != nullptr && *body == *other_body;
  }

 private:
  ExpandedNodeId data_type_id_;
  std::any value_;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const ExtensionObject& extension_object) {
  return stream << "{data_type_id: " << extension_object.data_type_id() << "}";
}

}  // namespace opcua
