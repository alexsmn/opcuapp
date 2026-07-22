#include "opcua/transport/binary/codec_utils.h"

#include "opcua/base/utf_convert.h"

#include <cstring>

namespace opcua::binary {
namespace {

// The built-in types a Variant can hold, paired with the C++ type of the
// alternative that holds them. Every Variant code path below — scalar encode,
// array encode, scalar decode, array decode — is generated from this one list,
// so a built-in cannot be handled in one direction and forgotten in another.
// The wire id of each is the Variant::Type enumerator itself (OPC UA Part 6
// §5.1.2 Built-in Types,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2).
//
// Each row is (enumerator, scalar alternative, array element). The two type
// columns differ only for VARIANT: a Variant nests a scalar Variant behind a
// shared pointer (the type is recursive through itself), but an array of
// Variants holds them inline.
//
// EMPTY is excluded: a null Variant carries no body and a null *array* carries
// only its element count, so both are handled separately below.
#define OPCUA_VARIANT_BUILT_IN_TYPES(V)                 \
  V(BOOL, bool, bool)                                   \
  V(INT8, Int8, Int8)                                   \
  V(UINT8, UInt8, UInt8)                                \
  V(INT16, Int16, Int16)                                \
  V(UINT16, UInt16, UInt16)                             \
  V(INT32, Int32, Int32)                                \
  V(UINT32, UInt32, UInt32)                             \
  V(INT64, Int64, Int64)                                \
  V(UINT64, UInt64, UInt64)                             \
  V(FLOAT, Float, Float)                                \
  V(DOUBLE, Double, Double)                             \
  V(STRING, String, String)                             \
  V(DATE_TIME, DateTime, DateTime)                      \
  V(GUID, Guid, Guid)                                   \
  V(BYTE_STRING, ByteString, ByteString)                \
  V(XML_ELEMENT, XmlElement, XmlElement)                \
  V(NODE_ID, NodeId, NodeId)                            \
  V(EXPANDED_NODE_ID, ExpandedNodeId, ExpandedNodeId)   \
  V(STATUS_CODE, Status, Status)                        \
  V(QUALIFIED_NAME, QualifiedName, QualifiedName)       \
  V(LOCALIZED_TEXT, LocalizedText, LocalizedText)       \
  V(EXTENSION_OBJECT, ExtensionObject, ExtensionObject) \
  V(DATA_VALUE, SharedDataValue, SharedDataValue)       \
  V(VARIANT, SharedVariant, Variant)                    \
  V(DIAGNOSTIC_INFO, DiagnosticInfo, DiagnosticInfo)

// A Variant nests DataValue and Variant behind shared pointers, since both are
// recursive through it.
using SharedDataValue = std::shared_ptr<const DataValue>;
using SharedVariant = std::shared_ptr<const Variant>;

void AppendExtensionObjectValue(Encoder& encoder,
                                const ExtensionObject& value) {
  const ByteString* body = value.binary_body();
  encoder.Encode(value.data_type_id());
  // OPC UA Part 6 §5.2.2.15: encoding byte 0x00 means "no body" and is NOT
  // followed by a length; 0x01 means a ByteString body follows. An
  // ExtensionObject with no value (the default) has no body, so it must encode
  // as 0x00 — writing 0x01 + a zero length instead would differ from the
  // null-body form the rest of the stack emits (e.g. an absent RequestHeader
  // additionalHeader) and waste four bytes.
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.15
  if (body == nullptr) {
    encoder.Encode(std::uint8_t{0x00});
    return;
  }
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(static_cast<std::int32_t>(body->size()));
  encoder.bytes().insert(encoder.bytes().end(), body->begin(), body->end());
}

bool ReadExtensionObjectValue(Decoder& decoder, ExtensionObject& value) {
  ExpandedNodeId data_type_id;
  std::uint8_t encoding = 0;
  std::int32_t length = 0;
  if (!decoder.Decode(data_type_id) || !decoder.Decode(encoding)) {
    return false;
  }
  if (encoding == 0x00) {
    value = ExtensionObject{std::move(data_type_id), ByteString{}};
    return true;
  }
  if (!decoder.Decode(length) || length < 0 ||
      decoder.remaining().size() < static_cast<std::size_t>(length)) {
    return false;
  }
  ByteString body(decoder.remaining().begin(),
                  decoder.remaining().begin() + length);
  decoder =
      Decoder{decoder.remaining().subspan(static_cast<std::size_t>(length))};
  value = ExtensionObject{std::move(data_type_id), std::move(body)};
  return true;
}

template <class T, class Writer>
void AppendArray(Encoder& encoder,
                 const std::vector<T>& values,
                 Writer&& writer) {
  encoder.Encode(static_cast<std::int32_t>(values.size()));
  for (const auto& value : values) {
    writer(value);
  }
}

// Safety cap on the element count of a Null (EMPTY) Variant array, whose
// elements occupy no wire bytes and so cannot be bounded by the remaining
// buffer. Guards against an allocation decode bomb.
constexpr std::int32_t kMaxNullArrayElements = 1 << 24;

template <class T, class Reader>
bool ReadArray(Decoder& decoder, std::vector<T>& values, Reader&& reader) {
  std::int32_t count = 0;
  if (!decoder.Decode(count) || count < 0) {
    return false;
  }
  // Every encoded element occupies at least one byte, so an array cannot have
  // more elements than the bytes remaining. Rejecting a larger count bounds the
  // reservation against a malformed/hostile length (decode bomb). OPC UA Part 6
  // §5.1.2 Decoding Errors,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2
  if (static_cast<std::size_t>(count) > decoder.remaining().size()) {
    return false;
  }
  values.clear();
  values.reserve(static_cast<std::size_t>(count));
  for (std::int32_t i = 0; i < count; ++i) {
    T value;
    if (!reader(value)) {
      return false;
    }
    values.push_back(std::move(value));
  }
  return true;
}

// Element codecs. Most delegate straight to the matching Encoder/Decoder
// overload; the ones spelled out here either have no such overload (the signed
// integers share their unsigned counterpart's wire form) or need indirection.
template <class T>
void EncodeElement(Encoder& encoder, const T& value) {
  encoder.Encode(value);
}

template <>
void EncodeElement(Encoder& encoder, const Int8& value) {
  encoder.Encode(static_cast<std::uint8_t>(value));
}

template <>
void EncodeElement(Encoder& encoder, const Int16& value) {
  encoder.Encode(static_cast<std::uint16_t>(value));
}

template <>
void EncodeElement(Encoder& encoder, const ExtensionObject& value) {
  AppendExtensionObjectValue(encoder, value);
}

// A null nested value has no wire representation, so it is written as the
// neutral value of its type: an empty DataValue / a null Variant.
template <>
void EncodeElement(Encoder& encoder, const SharedDataValue& value) {
  encoder.Encode(value ? *value : DataValue{});
}

template <>
void EncodeElement(Encoder& encoder, const SharedVariant& value) {
  encoder.Encode(value ? *value : Variant{});
}

template <class T>
bool DecodeElement(Decoder& decoder, T& value) {
  return decoder.Decode(value);
}

template <>
bool DecodeElement(Decoder& decoder, Int8& value) {
  std::uint8_t raw = 0;
  if (!decoder.Decode(raw))
    return false;
  value = static_cast<Int8>(raw);
  return true;
}

template <>
bool DecodeElement(Decoder& decoder, Int16& value) {
  std::uint16_t raw = 0;
  if (!decoder.Decode(raw))
    return false;
  value = static_cast<Int16>(raw);
  return true;
}

template <>
bool DecodeElement(Decoder& decoder, ExtensionObject& value) {
  return ReadExtensionObjectValue(decoder, value);
}

template <>
bool DecodeElement(Decoder& decoder, SharedDataValue& value) {
  DataValue decoded;
  if (!decoder.Decode(decoded))
    return false;
  value = std::make_shared<const DataValue>(std::move(decoded));
  return true;
}

template <>
bool DecodeElement(Decoder& decoder, SharedVariant& value) {
  Variant decoded;
  if (!decoder.Decode(decoded))
    return false;
  value = std::make_shared<const Variant>(std::move(decoded));
  return true;
}

}  // namespace

void Encoder::Encode(std::uint8_t value) {
  bytes_.push_back(static_cast<char>(value));
}

void Encoder::Encode(std::uint16_t value) {
  bytes_.push_back(static_cast<char>(value & 0xff));
  bytes_.push_back(static_cast<char>((value >> 8) & 0xff));
}

void Encoder::Encode(std::uint32_t value) {
  bytes_.push_back(static_cast<char>(value & 0xff));
  bytes_.push_back(static_cast<char>((value >> 8) & 0xff));
  bytes_.push_back(static_cast<char>((value >> 16) & 0xff));
  bytes_.push_back(static_cast<char>((value >> 24) & 0xff));
}

void Encoder::Encode(std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    bytes_.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  }
}

void Encoder::Encode(bool value) {
  Encode(static_cast<std::uint8_t>(value ? 1 : 0));
}

void Encoder::Encode(std::int32_t value) {
  Encode(static_cast<std::uint32_t>(value));
}

void Encoder::Encode(std::int64_t value) {
  Encode(static_cast<std::uint64_t>(value));
}

void Encoder::Encode(float value) {
  const auto* raw = reinterpret_cast<const char*>(&value);
  bytes_.insert(bytes_.end(), raw, raw + sizeof(value));
}

void Encoder::Encode(double value) {
  const auto* raw = reinterpret_cast<const char*>(&value);
  bytes_.insert(bytes_.end(), raw, raw + sizeof(value));
}

void Encoder::Encode(std::string_view value) {
  Encode(static_cast<std::int32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void Encoder::Encode(const String& value) {
  Encode(std::string_view{value});
}

void Encoder::Encode(const QualifiedName& value) {
  Encode(value.namespace_index());
  Encode(value.name());
}

void Encoder::Encode(const LocalizedText& value) {
  // OPC UA Part 6 §5.2.2.14 LocalizedText: an encoding-mask byte (bit 0 =
  // Locale present, bit 1 = Text present) followed by the present fields as
  // UTF-8 Strings,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.14
  std::uint8_t mask = 0;
  if (!value.locale.empty())
    mask |= 0x01;
  if (!value.text.empty())
    mask |= 0x02;
  Encode(mask);
  if ((mask & 0x01) != 0)
    Encode(value.locale);
  if ((mask & 0x02) != 0)
    Encode(UtfConvert<char>(value.text));
}

void Encoder::Encode(DateTime value) {
  Encode(value.ToInternalValue());
}

void Encoder::Encode(const Guid& value) {
  // OPC UA Part 6 §5.2.2.6 Guid: Data1/Data2/Data3 as little-endian integers
  // followed by Data4's eight bytes in order,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.6
  Encode(value.data1);
  Encode(value.data2);
  Encode(value.data3);
  bytes_.insert(bytes_.end(), value.data4.begin(), value.data4.end());
}

void Encoder::Encode(const ByteString& value) {
  Encode(static_cast<std::int32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void Encoder::Encode(const XmlElement& value) {
  // OPC UA Part 6 §5.2.2.8 XmlElement: the UTF-8 text encoded as a ByteString,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.8
  Encode(std::string_view{value.value});
}

void Encoder::Encode(const NodeId& node_id) {
  // OPC UA Part 6 §5.2.2.9 NodeId, Table 6,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.9: the
  // TwoByte format is the encoding byte 0x00 followed by a one-byte
  // identifier; a null NodeId is a TwoByte NodeId with identifier 0. The
  // encoding byte alone is not a valid NodeId.
  if (node_id.is_numeric() && node_id.namespace_index() == 0 &&
      node_id.numeric_id() <= 0xff) {
    Encode(std::uint8_t{0x00});
    Encode(static_cast<std::uint8_t>(node_id.numeric_id()));
    return;
  }
  if (node_id.is_numeric() && node_id.namespace_index() <= 0xff &&
      node_id.numeric_id() <= 0xffff) {
    Encode(std::uint8_t{0x01});
    Encode(static_cast<std::uint8_t>(node_id.namespace_index()));
    Encode(static_cast<std::uint16_t>(node_id.numeric_id()));
    return;
  }
  if (node_id.is_numeric()) {
    Encode(std::uint8_t{0x02});
    Encode(node_id.namespace_index());
    Encode(node_id.numeric_id());
    return;
  }
  if (node_id.is_string()) {
    Encode(std::uint8_t{0x03});
    Encode(node_id.namespace_index());
    Encode(node_id.string_id());
    return;
  }
  if (node_id.is_guid()) {
    Encode(std::uint8_t{0x04});
    Encode(node_id.namespace_index());
    Encode(node_id.guid_id());
    return;
  }
  Encode(std::uint8_t{0x05});
  Encode(node_id.namespace_index());
  Encode(node_id.opaque_id());
}

void Encoder::Encode(const ExpandedNodeId& node_id) {
  std::uint8_t encoding = 0x00;
  if (!node_id.namespace_uri().empty()) {
    encoding |= 0x80;
  }
  if (node_id.server_index() != 0) {
    encoding |= 0x40;
  }

  const auto* numeric =
      node_id.node_id().is_numeric() ? &node_id.node_id() : nullptr;
  // OPC UA Part 6 §5.2.2.9 Table 6: TwoByte (0x00) carries a one-byte
  // identifier; the null NodeId encodes as TwoByte with identifier 0.
  if (numeric != nullptr && numeric->namespace_index() == 0 &&
      numeric->numeric_id() <= 0xff) {
    encoding |= 0x00;
  } else if (numeric != nullptr && numeric->namespace_index() <= 0xff &&
             numeric->numeric_id() <= 0xffff) {
    encoding |= 0x01;
  } else if (numeric != nullptr) {
    encoding |= 0x02;
  } else if (node_id.node_id().is_string()) {
    encoding |= 0x03;
  } else {
    encoding |= 0x05;
  }

  Encode(encoding);
  if ((encoding & 0x3f) == 0x00) {
    Encode(static_cast<std::uint8_t>(node_id.node_id().numeric_id()));
  } else if ((encoding & 0x3f) == 0x01) {
    Encode(static_cast<std::uint8_t>(node_id.node_id().namespace_index()));
    Encode(static_cast<std::uint16_t>(node_id.node_id().numeric_id()));
  } else if ((encoding & 0x3f) == 0x02) {
    Encode(node_id.node_id().namespace_index());
    Encode(node_id.node_id().numeric_id());
  } else if ((encoding & 0x3f) == 0x03) {
    Encode(node_id.node_id().namespace_index());
    Encode(node_id.node_id().string_id());
  } else if ((encoding & 0x3f) == 0x05) {
    Encode(node_id.node_id().namespace_index());
    Encode(node_id.node_id().opaque_id());
  }
  if ((encoding & 0x80) != 0) {
    Encode(node_id.namespace_uri());
  }
  if ((encoding & 0x40) != 0) {
    Encode(node_id.server_index());
  }
}

void Encoder::Encode(Status value) {
  Encode(value.full_code());
}

void Encoder::Encode(const DiagnosticInfo& value) {
  // OPC UA Part 6 §5.2.2.13 DiagnosticInfo: an encoding-mask byte followed by
  // the present fields,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.13. Note
  // that the mask bit order and the payload order differ: LocalizedText owns
  // bit 2 and Locale bit 3, but Locale is written first.
  std::uint8_t mask = 0;
  if (value.symbolic_id.has_value())
    mask |= 0x01;
  if (value.namespace_uri.has_value())
    mask |= 0x02;
  if (value.localized_text.has_value())
    mask |= 0x04;
  if (value.locale.has_value())
    mask |= 0x08;
  if (value.additional_info.has_value())
    mask |= 0x10;
  if (value.inner_status_code.has_value())
    mask |= 0x20;
  if (value.inner_diagnostic_info != nullptr)
    mask |= 0x40;

  Encode(mask);
  if (value.symbolic_id.has_value())
    Encode(*value.symbolic_id);
  if (value.namespace_uri.has_value())
    Encode(*value.namespace_uri);
  if (value.locale.has_value())
    Encode(*value.locale);
  if (value.localized_text.has_value())
    Encode(*value.localized_text);
  if (value.additional_info.has_value())
    Encode(*value.additional_info);
  if (value.inner_status_code.has_value())
    Encode(*value.inner_status_code);
  if (value.inner_diagnostic_info != nullptr)
    Encode(*value.inner_diagnostic_info);
}

void Encoder::Encode(const DataValue& value) {
  // OPC UA Part 6 §5.2.2.17 DataValue,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.17. The
  // picoseconds fields (mask bits 4 and 5) are never written: opcuapp's
  // DataValue has no sub-100ns resolution to carry.
  std::uint8_t mask = 0;
  if (!value.value.is_null())
    mask |= 0x01;
  if (!IsGood(value.status_code))
    mask |= 0x02;
  if (!value.source_timestamp.is_null())
    mask |= 0x04;
  if (!value.server_timestamp.is_null())
    mask |= 0x08;

  Encode(mask);
  if ((mask & 0x01) != 0)
    Encode(value.value);
  if ((mask & 0x02) != 0)
    Encode(Status{value.status_code});
  if ((mask & 0x04) != 0)
    Encode(value.source_timestamp);
  if ((mask & 0x08) != 0)
    Encode(value.server_timestamp);
}

void Encoder::Encode(const Variant& value) {
  // OPC UA Part 6 §5.2.2.16 Variant: an encoding-mask byte carrying the
  // BuiltInType id in bits 0-5 and the array flag in bit 7, followed by the
  // value (or Int32 length + elements for an array),
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.16
  // An array of null elements also reports type() == EMPTY, so the array case
  // has to be tested before the null case — otherwise its element count would
  // be dropped and a bare null Variant would go out instead.
  const auto type_id = static_cast<std::uint8_t>(value.type());
  if (value.is_array()) {
    Encode(static_cast<std::uint8_t>(type_id | 0x80));
    switch (value.type()) {
      case Variant::EMPTY:
        // Null elements occupy no bytes, so only the count goes on the wire.
        Encode(static_cast<std::int32_t>(
            value.get<std::vector<std::monostate>>().size()));
        return;
#define OPCUA_ENCODE_ARRAY(NAME, SCALAR, ELEMENT)         \
  case Variant::NAME:                                     \
    AppendArray(*this, value.get<std::vector<ELEMENT>>(), \
                [&](const ELEMENT& element) {             \
                  EncodeElement<ELEMENT>(*this, element); \
                });                                       \
    return;
        OPCUA_VARIANT_BUILT_IN_TYPES(OPCUA_ENCODE_ARRAY)
#undef OPCUA_ENCODE_ARRAY
      case Variant::COUNT:
        Encode(std::int32_t{0});
        return;
    }
    return;
  }

  if (value.is_null()) {
    Encode(std::uint8_t{0});
    return;
  }

  Encode(type_id);
  switch (value.type()) {
    case Variant::EMPTY:
      return;
#define OPCUA_ENCODE_SCALAR(NAME, SCALAR, ELEMENT)     \
  case Variant::NAME:                                  \
    EncodeElement<SCALAR>(*this, value.get<SCALAR>()); \
    return;
      OPCUA_VARIANT_BUILT_IN_TYPES(OPCUA_ENCODE_SCALAR)
#undef OPCUA_ENCODE_SCALAR
    case Variant::COUNT:
      return;
  }
}

void Encoder::Encode(const ExtensionObject& value) {
  AppendExtensionObjectValue(*this, value);
}

bool Decoder::Decode(ExtensionObject& value) {
  return ReadExtensionObjectValue(*this, value);
}

void Encoder::Encode(const EncodedExtensionObject& value) {
  Encode(NodeId{value.type_id});
  Encode(std::uint8_t{0x01});
  Encode(static_cast<std::int32_t>(value.body.size()));
  bytes_.insert(bytes_.end(), value.body.begin(), value.body.end());
}

bool Decoder::Decode(std::uint8_t& value) {
  if (offset_ + 1 > bytes_.size()) {
    return false;
  }
  value = static_cast<std::uint8_t>(bytes_[offset_]);
  ++offset_;
  return true;
}

bool Decoder::Decode(std::uint16_t& value) {
  if (offset_ + 2 > bytes_.size()) {
    return false;
  }
  value = static_cast<std::uint16_t>(
      static_cast<unsigned char>(bytes_[offset_]) |
      (static_cast<std::uint16_t>(
           static_cast<unsigned char>(bytes_[offset_ + 1]))
       << 8));
  offset_ += 2;
  return true;
}

bool Decoder::Decode(std::uint32_t& value) {
  if (offset_ + 4 > bytes_.size()) {
    return false;
  }
  value =
      static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_])) |
      (static_cast<std::uint32_t>(
           static_cast<unsigned char>(bytes_[offset_ + 1]))
       << 8) |
      (static_cast<std::uint32_t>(
           static_cast<unsigned char>(bytes_[offset_ + 2]))
       << 16) |
      (static_cast<std::uint32_t>(
           static_cast<unsigned char>(bytes_[offset_ + 3]))
       << 24);
  offset_ += 4;
  return true;
}

bool Decoder::Decode(bool& value) {
  std::uint8_t raw = 0;
  if (!Decode(raw)) {
    return false;
  }
  value = raw != 0;
  return true;
}

bool Decoder::Decode(std::int32_t& value) {
  std::uint32_t raw = 0;
  if (!Decode(raw)) {
    return false;
  }
  value = static_cast<std::int32_t>(raw);
  return true;
}

bool Decoder::Decode(std::int64_t& value) {
  if (offset_ + 8 > bytes_.size()) {
    return false;
  }
  std::uint64_t raw = 0;
  for (int i = 0; i < 8; ++i) {
    raw |= static_cast<std::uint64_t>(
               static_cast<unsigned char>(bytes_[offset_ + i]))
           << (8 * i);
  }
  value = static_cast<std::int64_t>(raw);
  offset_ += 8;
  return true;
}

bool Decoder::Decode(std::uint64_t& value) {
  std::int64_t raw = 0;
  if (!Decode(raw)) {
    return false;
  }
  value = static_cast<std::uint64_t>(raw);
  return true;
}

bool Decoder::Decode(float& value) {
  if (offset_ + sizeof(float) > bytes_.size()) {
    return false;
  }
  std::memcpy(&value, bytes_.data() + offset_, sizeof(value));
  offset_ += sizeof(value);
  return true;
}

bool Decoder::Decode(double& value) {
  if (offset_ + sizeof(double) > bytes_.size()) {
    return false;
  }
  std::memcpy(&value, bytes_.data() + offset_, sizeof(value));
  offset_ += sizeof(value);
  return true;
}

bool Decoder::Decode(String& value) {
  std::int32_t length = 0;
  if (!Decode(length)) {
    return false;
  }
  if (length < 0) {
    value.clear();
    return true;
  }
  if (offset_ + static_cast<std::size_t>(length) > bytes_.size()) {
    return false;
  }
  value.assign(bytes_.data() + offset_, static_cast<std::size_t>(length));
  offset_ += static_cast<std::size_t>(length);
  return true;
}

bool Decoder::Decode(QualifiedName& value) {
  std::uint16_t namespace_index = 0;
  std::string name;
  if (!Decode(namespace_index) || !Decode(name)) {
    return false;
  }
  value = QualifiedName{std::move(name), namespace_index};
  return true;
}

bool Decoder::Decode(LocalizedText& value) {
  // OPC UA Part 6 §5.2.2.14 LocalizedText,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.14
  std::uint8_t mask = 0;
  if (!Decode(mask)) {
    return false;
  }
  String locale;
  if ((mask & 0x01) != 0 && !Decode(locale)) {
    return false;
  }
  std::string text;
  if ((mask & 0x02) != 0 && !Decode(text)) {
    return false;
  }
  value = LocalizedText{std::move(locale), UtfConvert<char16_t>(text)};
  return true;
}

bool Decoder::Decode(DateTime& value) {
  std::int64_t raw = 0;
  if (!Decode(raw)) {
    return false;
  }
  value = DateTime::FromInternalValue(raw);
  return true;
}

bool Decoder::Decode(Guid& value) {
  // OPC UA Part 6 §5.2.2.6 Guid,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.6
  if (!Decode(value.data1) || !Decode(value.data2) || !Decode(value.data3))
    return false;
  if (offset_ + value.data4.size() > bytes_.size())
    return false;
  for (std::uint8_t& byte : value.data4) {
    if (!Decode(byte))
      return false;
  }
  return true;
}

bool Decoder::Decode(XmlElement& value) {
  // OPC UA Part 6 §5.2.2.8 XmlElement: a ByteString holding the UTF-8 text,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.8
  return Decode(value.value);
}

bool Decoder::Decode(Status& value) {
  std::uint32_t full_code = 0;
  if (!Decode(full_code))
    return false;
  value = Status::FromFullCode(full_code);
  return true;
}

bool Decoder::Decode(DiagnosticInfo& value) {
  // OPC UA Part 6 §5.2.2.13 DiagnosticInfo,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.13. The
  // mask bit order and the payload order differ for Locale/LocalizedText — see
  // the matching encoder.
  std::uint8_t mask = 0;
  if (!Decode(mask))
    return false;

  value = DiagnosticInfo{};
  const auto decode_index = [&](std::optional<Int32>& field) {
    Int32 index = 0;
    if (!Decode(index))
      return false;
    field = index;
    return true;
  };

  if ((mask & 0x01) != 0 && !decode_index(value.symbolic_id))
    return false;
  if ((mask & 0x02) != 0 && !decode_index(value.namespace_uri))
    return false;
  if ((mask & 0x08) != 0 && !decode_index(value.locale))
    return false;
  if ((mask & 0x04) != 0 && !decode_index(value.localized_text))
    return false;
  if ((mask & 0x10) != 0) {
    String additional_info;
    if (!Decode(additional_info))
      return false;
    value.additional_info = std::move(additional_info);
  }
  if ((mask & 0x20) != 0) {
    Status inner_status_code{StatusCode::Good};
    if (!Decode(inner_status_code))
      return false;
    value.inner_status_code = inner_status_code;
  }
  if ((mask & 0x40) != 0) {
    DiagnosticInfo inner;
    if (!Decode(inner))
      return false;
    value.inner_diagnostic_info =
        std::make_shared<const DiagnosticInfo>(std::move(inner));
  }
  return true;
}

bool Decoder::Decode(DataValue& value) {
  // OPC UA Part 6 §5.2.2.17 DataValue,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.17. The
  // picoseconds fields are decoded and discarded rather than rejected:
  // opcuapp's DataValue cannot hold sub-100ns resolution, but a peer that
  // sends them is still speaking the standard encoding and the rest of the
  // frame has to stay parseable.
  std::uint8_t mask = 0;
  if (!Decode(mask) || (mask & 0xc0) != 0)
    return false;

  value = DataValue{};
  if ((mask & 0x01) != 0 && !Decode(value.value))
    return false;
  if ((mask & 0x02) != 0) {
    Status status{StatusCode::Good};
    if (!Decode(status))
      return false;
    value.status_code = status.code();
  }
  if ((mask & 0x04) != 0 && !Decode(value.source_timestamp))
    return false;
  std::uint16_t picoseconds = 0;
  if ((mask & 0x10) != 0 && !Decode(picoseconds))
    return false;
  if ((mask & 0x08) != 0 && !Decode(value.server_timestamp))
    return false;
  if ((mask & 0x20) != 0 && !Decode(picoseconds))
    return false;
  return true;
}

bool Decoder::Decode(ByteString& value) {
  std::int32_t length = 0;
  if (!Decode(length)) {
    return false;
  }
  if (length < 0) {
    value.clear();
    return true;
  }
  if (offset_ + static_cast<std::size_t>(length) > bytes_.size()) {
    return false;
  }
  value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
               bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
  offset_ += static_cast<std::size_t>(length);
  return true;
}

bool Decoder::Decode(NodeId& id) {
  std::uint8_t encoding = 0;
  if (!Decode(encoding)) {
    return false;
  }
  // OPC UA Part 6 §5.2.2.9 NodeId, Table 6,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.9: TwoByte
  // is the encoding byte followed by a one-byte identifier (ns 0). An
  // identifier of 0 yields the null NodeId.
  if (encoding == 0x00) {
    std::uint8_t short_id = 0;
    if (!Decode(short_id)) {
      return false;
    }
    id = NodeId{short_id, 0};
    return true;
  }
  if (encoding == 0x01) {
    std::uint8_t ns = 0;
    std::uint16_t short_id = 0;
    if (!Decode(ns) || !Decode(short_id)) {
      return false;
    }
    id = NodeId{short_id, ns};
    return true;
  }
  if (encoding == 0x02) {
    std::uint16_t ns = 0;
    std::uint32_t numeric_id = 0;
    if (!Decode(ns) || !Decode(numeric_id)) {
      return false;
    }
    id = NodeId{numeric_id, ns};
    return true;
  }
  if (encoding == 0x03) {
    std::uint16_t ns = 0;
    String string_id;
    if (!Decode(ns) || !Decode(string_id)) {
      return false;
    }
    id = NodeId{std::move(string_id), ns};
    return true;
  }
  if (encoding == 0x04) {
    std::uint16_t ns = 0;
    Guid guid_id;
    if (!Decode(ns) || !Decode(guid_id)) {
      return false;
    }
    id = NodeId{guid_id, ns};
    return true;
  }
  if (encoding == 0x05) {
    std::uint16_t ns = 0;
    ByteString opaque_id;
    if (!Decode(ns) || !Decode(opaque_id)) {
      return false;
    }
    id = NodeId{std::move(opaque_id), ns};
    return true;
  }
  return false;
}

bool Decoder::Decode(ExpandedNodeId& id) {
  std::uint8_t encoding = 0;
  if (!Decode(encoding)) {
    return false;
  }

  NodeId node_id;
  switch (encoding & 0x3f) {
    // OPC UA Part 6 §5.2.2.9 Table 6: TwoByte carries a one-byte identifier.
    case 0x00: {
      std::uint8_t short_id = 0;
      if (!Decode(short_id)) {
        return false;
      }
      node_id = NodeId{short_id, 0};
      break;
    }
    case 0x01: {
      std::uint8_t ns = 0;
      std::uint16_t short_id = 0;
      if (!Decode(ns) || !Decode(short_id)) {
        return false;
      }
      node_id = NodeId{short_id, ns};
      break;
    }
    case 0x02: {
      std::uint16_t ns = 0;
      std::uint32_t numeric_id = 0;
      if (!Decode(ns) || !Decode(numeric_id)) {
        return false;
      }
      node_id = NodeId{numeric_id, ns};
      break;
    }
    case 0x03: {
      std::uint16_t ns = 0;
      String string_id;
      if (!Decode(ns) || !Decode(string_id)) {
        return false;
      }
      node_id = NodeId{std::move(string_id), ns};
      break;
    }
    case 0x05: {
      std::uint16_t ns = 0;
      ByteString opaque_id;
      if (!Decode(ns) || !Decode(opaque_id)) {
        return false;
      }
      node_id = NodeId{std::move(opaque_id), ns};
      break;
    }
    default:
      return false;
  }

  std::string namespace_uri;
  if ((encoding & 0x80) != 0 && !Decode(namespace_uri)) {
    return false;
  }

  std::uint32_t server_index = 0;
  if ((encoding & 0x40) != 0 && !Decode(server_index)) {
    return false;
  }

  id = ExpandedNodeId{std::move(node_id), std::move(namespace_uri),
                      server_index};
  return true;
}

bool Decoder::Decode(Variant& value) {
  // OPC UA Part 6 §5.2.2.16 Variant,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.16. Bit 6
  // signals array dimensions (a matrix), which opcuapp does not model; such a
  // Variant is rejected rather than silently decoded as a flat array.
  std::uint8_t encoding_mask = 0;
  if (!Decode(encoding_mask))
    return false;

  const bool is_array = (encoding_mask & 0x80) != 0;
  const bool has_dimensions = (encoding_mask & 0x40) != 0;
  if (has_dimensions)
    return false;

  const auto type_id = static_cast<unsigned>(encoding_mask & 0x3f);
  if (type_id >= static_cast<unsigned>(Variant::COUNT))
    return false;
  const auto type = static_cast<Variant::Type>(type_id);

  if (is_array) {
    switch (type) {
      case Variant::EMPTY: {
        std::int32_t count = 0;
        // Null array elements carry no wire bytes, so bound the count by a
        // fixed cap rather than the remaining buffer (decode bomb guard).
        if (!Decode(count) || count < 0 || count > kMaxNullArrayElements)
          return false;
        value = Variant{
            std::vector<std::monostate>(static_cast<std::size_t>(count))};
        return true;
      }
#define OPCUA_DECODE_ARRAY(NAME, SCALAR, ELEMENT)           \
  case Variant::NAME: {                                     \
    std::vector<ELEMENT> elements;                          \
    if (!ReadArray(*this, elements, [&](ELEMENT& element) { \
          return DecodeElement<ELEMENT>(*this, element);    \
        })) {                                               \
      return false;                                         \
    }                                                       \
    value = Variant{std::move(elements)};                   \
    return true;                                            \
  }
        OPCUA_VARIANT_BUILT_IN_TYPES(OPCUA_DECODE_ARRAY)
#undef OPCUA_DECODE_ARRAY
      case Variant::COUNT:
        return false;
    }
    return false;
  }

  switch (type) {
    case Variant::EMPTY:
      value = Variant{};
      return true;
#define OPCUA_DECODE_SCALAR(NAME, SCALAR, ELEMENT) \
  case Variant::NAME: {                            \
    SCALAR element{};                              \
    if (!DecodeElement<SCALAR>(*this, element))    \
      return false;                                \
    value = Variant{std::move(element)};           \
    return true;                                   \
  }
      OPCUA_VARIANT_BUILT_IN_TYPES(OPCUA_DECODE_SCALAR)
#undef OPCUA_DECODE_SCALAR
    case Variant::COUNT:
      return false;
  }

  return false;
}

bool Decoder::Decode(DecodedExtensionObject& value) {
  NodeId node_id;
  if (!Decode(node_id) || !node_id.is_numeric()) {
    return false;
  }
  value.type_id = node_id.numeric_id();
  if (!Decode(value.encoding)) {
    return false;
  }
  if (value.encoding == 0x00) {
    value.body.clear();
    return true;
  }
  std::int32_t length = 0;
  if (!Decode(length) || length < 0 ||
      offset_ + static_cast<std::size_t>(length) > bytes_.size()) {
    return false;
  }
  value.body.assign(
      bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
      bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
  offset_ += static_cast<std::size_t>(length);
  return true;
}

bool Decoder::Skip(std::size_t count) {
  if (remaining().size() < count) {
    return false;
  }
  offset_ += count;
  return true;
}

// OPC UA Part 6 §6.7 (Message structure) / §5.2.2.15 (encoding of Messages),
// https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7: the body of a
// MessageChunk is the NodeId of the message's DataTypeEncoding node followed
// directly by the encoded message structure. Unlike an ExtensionObject field
// there is no encoding-mask byte and no length prefix; the message extends to
// the end of the (reassembled) chunk body.
void AppendMessage(Encoder& encoder,
                   std::uint32_t type_id,
                   std::span<const char> payload) {
  encoder.Encode(NodeId{type_id});
  encoder.bytes().insert(encoder.bytes().end(), payload.begin(), payload.end());
}

std::optional<std::pair<std::uint32_t, std::span<const char>>> ReadMessage(
    Decoder& decoder) {
  NodeId type_id;
  if (!decoder.Decode(type_id) || !type_id.is_numeric() ||
      type_id.namespace_index() != 0) {
    return std::nullopt;
  }
  const auto payload = decoder.remaining();
  if (!decoder.Skip(payload.size())) {
    return std::nullopt;
  }
  return std::pair{type_id.numeric_id(), payload};
}

}  // namespace opcua::binary
