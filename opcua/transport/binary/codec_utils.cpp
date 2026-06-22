#include "opcua/transport/binary/codec_utils.h"

#include <cstring>

namespace opcua::binary {
namespace {

unsigned BuiltInTypeId(Variant::Type type) {
  switch (type) {
    case Variant::EMPTY: return 0;
    case Variant::BOOL: return 1;
    case Variant::INT8: return 2;
    case Variant::UINT8: return 3;
    case Variant::INT16: return 4;
    case Variant::UINT16: return 5;
    case Variant::INT32: return 6;
    case Variant::UINT32: return 7;
    case Variant::INT64: return 8;
    case Variant::UINT64: return 9;
    case Variant::DOUBLE: return 11;
    case Variant::STRING: return 12;
    case Variant::DATE_TIME: return 13;
    case Variant::BYTE_STRING: return 15;
    case Variant::NODE_ID: return 17;
    case Variant::EXPANDED_NODE_ID: return 18;
    case Variant::QUALIFIED_NAME: return 20;
    case Variant::LOCALIZED_TEXT: return 21;
    case Variant::EXTENSION_OBJECT: return 22;
    case Variant::COUNT: return 0;
  }
  return 0;
}

Variant::Type FromBuiltInTypeId(unsigned id) {
  switch (id) {
    case 0: return Variant::EMPTY;
    case 1: return Variant::BOOL;
    case 2: return Variant::INT8;
    case 3: return Variant::UINT8;
    case 4: return Variant::INT16;
    case 5: return Variant::UINT16;
    case 6: return Variant::INT32;
    case 7: return Variant::UINT32;
    case 8: return Variant::INT64;
    case 9: return Variant::UINT64;
    case 11: return Variant::DOUBLE;
    case 12: return Variant::STRING;
    case 13: return Variant::DATE_TIME;
    case 15: return Variant::BYTE_STRING;
    case 17: return Variant::NODE_ID;
    case 18: return Variant::EXPANDED_NODE_ID;
    case 20: return Variant::QUALIFIED_NAME;
    case 21: return Variant::LOCALIZED_TEXT;
    case 22: return Variant::EXTENSION_OBJECT;
    default: return Variant::COUNT;
  }
}

void AppendExtensionObjectValue(Encoder& encoder,
                                const ExtensionObject& value) {
  std::vector<char> body;
  if (const auto* raw_bytes = std::any_cast<ByteString>(&value.value())) {
    body = *raw_bytes;
  } else if (const auto* raw_vector =
                 std::any_cast<std::vector<char>>(&value.value())) {
    body = *raw_vector;
  }
  encoder.Encode(value.data_type_id());
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(static_cast<std::int32_t>(body.size()));
  encoder.bytes().insert(encoder.bytes().end(), body.begin(), body.end());
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
  decoder = Decoder{decoder.remaining().subspan(static_cast<std::size_t>(length))};
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
bool ReadArray(Decoder& decoder,
               std::vector<T>& values,
               Reader&& reader) {
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

void Encoder::Encode(bool value) {
  Encode(static_cast<std::uint8_t>(value ? 1 : 0));
}

void Encoder::Encode(std::int32_t value) {
  Encode(static_cast<std::uint32_t>(value));
}

void Encoder::Encode(std::int64_t value) {
  const auto raw = static_cast<std::uint64_t>(value);
  for (int i = 0; i < 8; ++i) {
    bytes_.push_back(static_cast<char>((raw >> (8 * i)) & 0xff));
  }
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
  if (value.empty()) {
    Encode(std::uint8_t{0});
    return;
  }
  const auto utf8 = ToString(value);
  Encode(std::uint8_t{0x02});
  Encode(utf8);
}

void Encoder::Encode(DateTime value) {
  if (value.is_null()) {
    Encode(std::int64_t{0});
    return;
  }
  Encode(value.ToDeltaSinceWindowsEpoch().InMicroseconds() * 10);
}

void Encoder::Encode(const ByteString& value) {
  Encode(static_cast<std::int32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void Encoder::Encode(const NodeId& node_id) {
  if (node_id.is_null()) {
    Encode(std::uint8_t{0x00});
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
  if (node_id.node_id().is_null()) {
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
  if ((encoding & 0x3f) == 0x01) {
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

void Encoder::Encode(const Variant& value) {
  if (value.is_array()) {
    Encode(static_cast<std::uint8_t>(BuiltInTypeId(value.type()) | 0x80));
    switch (value.type()) {
      case Variant::EMPTY:
        Encode(static_cast<std::int32_t>(
            value.get<std::vector<std::monostate>>().size()));
        return;
      case Variant::BOOL:
        AppendArray(*this, value.get<std::vector<bool>>(),
                    [&](bool element) { Encode(element); });
        return;
      case Variant::INT8:
        AppendArray(*this, value.get<std::vector<Int8>>(),
                    [&](Int8 element) {
                      Encode(static_cast<std::uint8_t>(element));
                    });
        return;
      case Variant::UINT8:
        AppendArray(*this, value.get<std::vector<UInt8>>(),
                    [&](UInt8 element) { Encode(element); });
        return;
      case Variant::INT16:
        AppendArray(*this, value.get<std::vector<Int16>>(),
                    [&](Int16 element) {
                      Encode(static_cast<std::uint16_t>(element));
                    });
        return;
      case Variant::UINT16:
        AppendArray(*this, value.get<std::vector<UInt16>>(),
                    [&](UInt16 element) { Encode(element); });
        return;
      case Variant::INT32:
        AppendArray(*this, value.get<std::vector<Int32>>(),
                    [&](Int32 element) { Encode(element); });
        return;
      case Variant::UINT32:
        AppendArray(*this, value.get<std::vector<UInt32>>(),
                    [&](UInt32 element) { Encode(element); });
        return;
      case Variant::INT64:
        AppendArray(*this, value.get<std::vector<Int64>>(),
                    [&](Int64 element) { Encode(element); });
        return;
      case Variant::UINT64:
        AppendArray(*this, value.get<std::vector<UInt64>>(),
                    [&](UInt64 element) {
                      for (int i = 0; i < 8; ++i) {
                        Encode(static_cast<std::uint8_t>(
                            (element >> (8 * i)) & 0xff));
                      }
                    });
        return;
      case Variant::DOUBLE:
        AppendArray(*this, value.get<std::vector<Double>>(),
                    [&](Double element) { Encode(element); });
        return;
      case Variant::BYTE_STRING:
        AppendArray(*this, value.get<std::vector<ByteString>>(),
                    [&](const ByteString& element) { Encode(element); });
        return;
      case Variant::STRING:
        AppendArray(*this, value.get<std::vector<String>>(),
                    [&](const String& element) {
                      Encode(element);
                    });
        return;
      case Variant::QUALIFIED_NAME:
        AppendArray(*this, value.get<std::vector<QualifiedName>>(),
                    [&](const QualifiedName& element) { Encode(element); });
        return;
      case Variant::LOCALIZED_TEXT:
        AppendArray(*this, value.get<std::vector<LocalizedText>>(),
                    [&](const LocalizedText& element) { Encode(element); });
        return;
      case Variant::NODE_ID:
        AppendArray(*this, value.get<std::vector<NodeId>>(),
                    [&](const NodeId& element) { Encode(element); });
        return;
      case Variant::EXPANDED_NODE_ID:
        AppendArray(*this, value.get<std::vector<ExpandedNodeId>>(),
                    [&](const ExpandedNodeId& element) { Encode(element); });
        return;
      case Variant::EXTENSION_OBJECT:
        AppendArray(*this, value.get<std::vector<ExtensionObject>>(),
                    [&](const ExtensionObject& element) {
                      AppendExtensionObjectValue(*this, element);
                    });
        return;
      case Variant::DATE_TIME:
      case Variant::COUNT:
        Encode(std::int32_t{0});
        return;
    }
  }

  if (value.is_null()) {
    Encode(std::uint8_t{0});
    return;
  }
  Encode(static_cast<std::uint8_t>(BuiltInTypeId(value.type())));
  switch (value.type()) {
    case Variant::EMPTY:
      return;
    case Variant::BOOL:
      Encode(value.get<bool>());
      return;
    case Variant::INT8:
      Encode(static_cast<std::uint8_t>(value.get<Int8>()));
      return;
    case Variant::UINT8:
      Encode(value.get<UInt8>());
      return;
    case Variant::INT16:
      Encode(static_cast<std::uint16_t>(value.get<Int16>()));
      return;
    case Variant::UINT16:
      Encode(value.get<UInt16>());
      return;
    case Variant::INT32:
      Encode(value.get<Int32>());
      return;
    case Variant::UINT32:
      Encode(value.get<UInt32>());
      return;
    case Variant::INT64:
      Encode(value.get<Int64>());
      return;
    case Variant::UINT64: {
      const auto raw = value.get<UInt64>();
      for (int i = 0; i < 8; ++i) {
        Encode(static_cast<std::uint8_t>((raw >> (8 * i)) & 0xff));
      }
      return;
    }
    case Variant::DOUBLE:
      Encode(value.get<Double>());
      return;
    case Variant::BYTE_STRING:
      Encode(value.get<ByteString>());
      return;
    case Variant::STRING:
      Encode(value.get<String>());
      return;
    case Variant::QUALIFIED_NAME:
      Encode(value.get<QualifiedName>());
      return;
    case Variant::LOCALIZED_TEXT:
      Encode(value.get<LocalizedText>());
      return;
    case Variant::NODE_ID:
      Encode(value.get<NodeId>());
      return;
    case Variant::EXPANDED_NODE_ID:
      Encode(value.get<ExpandedNodeId>());
      return;
    case Variant::EXTENSION_OBJECT:
      AppendExtensionObjectValue(*this, value.get<ExtensionObject>());
      return;
    case Variant::DATE_TIME:
      Encode(value.get<DateTime>());
      return;
    case Variant::COUNT:
      Encode(std::uint8_t{0});
      return;
  }
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
      (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes_[offset_ + 1]))
       << 8));
  offset_ += 2;
  return true;
}

bool Decoder::Decode(std::uint32_t& value) {
  if (offset_ + 4 > bytes_.size()) {
    return false;
  }
  value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_])) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_ + 1])) << 8) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_ + 2])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_ + 3])) << 24);
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
  std::uint8_t mask = 0;
  if (!Decode(mask)) {
    return false;
  }
  if ((mask & 0x01) != 0) {
    std::string ignored_locale;
    if (!Decode(ignored_locale)) {
      return false;
    }
  }
  std::string text;
  if ((mask & 0x02) != 0 && !Decode(text)) {
    return false;
  }
  value = ToLocalizedText(text);
  return true;
}

bool Decoder::Decode(DateTime& value) {
  std::int64_t raw = 0;
  if (!Decode(raw)) {
    return false;
  }
  if (raw == 0) {
    value = {};
    return true;
  }
  value = base::Time::FromDeltaSinceWindowsEpoch(
      base::TimeDelta::FromMicroseconds(raw / 10));
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
  if (encoding == 0x00) {
    id = {};
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
    case 0x00:
      node_id = {};
      break;
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
  std::uint8_t encoding_mask = 0;
  if (!Decode(encoding_mask) || (encoding_mask & 0x80) != 0) {
    if ((encoding_mask & 0x80) == 0) {
      return false;
    }
  }

  const bool is_array = (encoding_mask & 0x80) != 0;
  const auto type = FromBuiltInTypeId(encoding_mask & 0x3f);
  if (type == Variant::COUNT) {
    return false;
  }

  if (is_array) {
    switch (type) {
      case Variant::EMPTY: {
        std::int32_t count = 0;
        // Null array elements carry no wire bytes, so bound the count by a
        // fixed cap rather than the remaining buffer (decode bomb guard).
        if (!Decode(count) || count < 0 || count > kMaxNullArrayElements) {
          return false;
        }
        value = Variant{std::vector<std::monostate>(
            static_cast<std::size_t>(count))};
        return true;
      }
      case Variant::BOOL: {
        std::vector<bool> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](bool& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::INT8: {
        std::vector<Int8> typed_values;
        if (!ReadArray(*this, typed_values, [&](Int8& element) {
              std::uint8_t raw = 0;
              if (!Decode(raw)) {
                return false;
              }
              element = static_cast<Int8>(raw);
              return true;
            })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::UINT8: {
        std::vector<UInt8> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](UInt8& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::INT16: {
        std::vector<Int16> typed_values;
        if (!ReadArray(*this, typed_values, [&](Int16& element) {
              std::uint16_t raw = 0;
              if (!Decode(raw)) {
                return false;
              }
              element = static_cast<Int16>(raw);
              return true;
            })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::UINT16: {
        std::vector<UInt16> typed_values;
        if (!ReadArray(*this, typed_values, [&](UInt16& element) {
              return Decode(element);
            })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::INT32: {
        std::vector<Int32> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](Int32& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::UINT32: {
        std::vector<UInt32> typed_values;
        if (!ReadArray(*this, typed_values, [&](UInt32& element) {
              return Decode(element);
            })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::INT64: {
        std::vector<Int64> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](Int64& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::UINT64: {
        std::vector<UInt64> typed_values;
        if (!ReadArray(*this, typed_values, [&](UInt64& element) {
              element = 0;
              for (int i = 0; i < 8; ++i) {
                std::uint8_t byte = 0;
                if (!Decode(byte)) {
                  return false;
                }
                element |= static_cast<std::uint64_t>(byte) << (8 * i);
              }
              return true;
            })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::DOUBLE: {
        std::vector<Double> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](Double& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::BYTE_STRING: {
        std::vector<ByteString> typed_values;
        if (!ReadArray(*this, typed_values, [&](ByteString& element) {
              return Decode(element);
            })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::STRING: {
        std::vector<String> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](String& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::QUALIFIED_NAME: {
        std::vector<QualifiedName> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](QualifiedName& element) {
                         return Decode(element);
                       })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::LOCALIZED_TEXT: {
        std::vector<LocalizedText> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](LocalizedText& element) {
                         return Decode(element);
                       })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::NODE_ID: {
        std::vector<NodeId> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](NodeId& element) { return Decode(element); })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::EXPANDED_NODE_ID: {
        std::vector<ExpandedNodeId> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](ExpandedNodeId& element) {
                         return Decode(element);
                       })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::EXTENSION_OBJECT: {
        std::vector<ExtensionObject> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](ExtensionObject& element) {
                         return ReadExtensionObjectValue(*this, element);
                       })) {
          return false;
        }
        value = Variant{std::move(typed_values)};
        return true;
      }
      case Variant::DATE_TIME:
      case Variant::COUNT:
        return false;
    }
  }

  switch (type) {
    case Variant::EMPTY:
      value = Variant{};
      return true;
    case Variant::BOOL: {
      bool typed_value = false;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::INT8: {
      std::uint8_t raw = 0;
      if (!Decode(raw)) return false;
      value = Variant{static_cast<Int8>(raw)};
      return true;
    }
    case Variant::UINT8: {
      UInt8 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::INT16: {
      std::uint16_t raw = 0;
      if (!Decode(raw)) return false;
      value = Variant{static_cast<Int16>(raw)};
      return true;
    }
    case Variant::UINT16: {
      UInt16 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::INT32: {
      Int32 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::UINT32: {
      UInt32 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::INT64: {
      Int64 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::UINT64: {
      UInt64 typed_value = 0;
      for (int i = 0; i < 8; ++i) {
        std::uint8_t byte = 0;
        if (!Decode(byte)) return false;
        typed_value |= static_cast<std::uint64_t>(byte) << (8 * i);
      }
      value = Variant{typed_value};
      return true;
    }
    case Variant::DOUBLE: {
      double typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
    case Variant::BYTE_STRING: {
      ByteString typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::STRING: {
      std::string typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::QUALIFIED_NAME: {
      QualifiedName typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::LOCALIZED_TEXT: {
      LocalizedText typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::NODE_ID: {
      NodeId typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::EXPANDED_NODE_ID: {
      ExpandedNodeId typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::EXTENSION_OBJECT: {
      ExtensionObject typed_value;
      if (!ReadExtensionObjectValue(*this, typed_value)) return false;
      value = Variant{std::move(typed_value)};
      return true;
    }
    case Variant::DATE_TIME: {
      DateTime typed_value;
      if (!Decode(typed_value)) return false;
      value = Variant{typed_value};
      return true;
    }
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
  value.body.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
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

void AppendMessage(Encoder& encoder,
                   std::uint32_t type_id,
                   std::span<const char> payload) {
  encoder.Encode(NodeId{type_id});
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(static_cast<std::int32_t>(payload.size()));
  encoder.bytes().insert(encoder.bytes().end(), payload.begin(), payload.end());
}

std::optional<std::pair<std::uint32_t, std::span<const char>>> ReadMessage(
    Decoder& decoder) {
  NodeId type_id;
  std::uint8_t encoding = 0;
  std::int32_t payload_size = 0;
  if (!decoder.Decode(type_id) || !type_id.is_numeric() ||
      !decoder.Decode(encoding) || encoding != 0x01 ||
      !decoder.Decode(payload_size) || payload_size < 0 ||
      decoder.remaining().size() < static_cast<std::size_t>(payload_size)) {
    return std::nullopt;
  }
  const auto payload = decoder.remaining().first(static_cast<std::size_t>(payload_size));
  if (!decoder.Skip(static_cast<std::size_t>(payload_size))) {
    return std::nullopt;
  }
  return std::pair{type_id.numeric_id(), payload};
}

}  // namespace opcua::binary
