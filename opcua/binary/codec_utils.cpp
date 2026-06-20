#include "opcua/binary/codec_utils.h"

#include <cstring>

namespace opcua::binary {
namespace {

unsigned BuiltInTypeId(scada::Variant::Type type) {
  switch (type) {
    case scada::Variant::EMPTY: return 0;
    case scada::Variant::BOOL: return 1;
    case scada::Variant::INT8: return 2;
    case scada::Variant::UINT8: return 3;
    case scada::Variant::INT16: return 4;
    case scada::Variant::UINT16: return 5;
    case scada::Variant::INT32: return 6;
    case scada::Variant::UINT32: return 7;
    case scada::Variant::INT64: return 8;
    case scada::Variant::UINT64: return 9;
    case scada::Variant::DOUBLE: return 11;
    case scada::Variant::STRING: return 12;
    case scada::Variant::DATE_TIME: return 13;
    case scada::Variant::BYTE_STRING: return 15;
    case scada::Variant::NODE_ID: return 17;
    case scada::Variant::EXPANDED_NODE_ID: return 18;
    case scada::Variant::QUALIFIED_NAME: return 20;
    case scada::Variant::LOCALIZED_TEXT: return 21;
    case scada::Variant::EXTENSION_OBJECT: return 22;
    case scada::Variant::COUNT: return 0;
  }
  return 0;
}

scada::Variant::Type FromBuiltInTypeId(unsigned id) {
  switch (id) {
    case 0: return scada::Variant::EMPTY;
    case 1: return scada::Variant::BOOL;
    case 2: return scada::Variant::INT8;
    case 3: return scada::Variant::UINT8;
    case 4: return scada::Variant::INT16;
    case 5: return scada::Variant::UINT16;
    case 6: return scada::Variant::INT32;
    case 7: return scada::Variant::UINT32;
    case 8: return scada::Variant::INT64;
    case 9: return scada::Variant::UINT64;
    case 11: return scada::Variant::DOUBLE;
    case 12: return scada::Variant::STRING;
    case 13: return scada::Variant::DATE_TIME;
    case 15: return scada::Variant::BYTE_STRING;
    case 17: return scada::Variant::NODE_ID;
    case 18: return scada::Variant::EXPANDED_NODE_ID;
    case 20: return scada::Variant::QUALIFIED_NAME;
    case 21: return scada::Variant::LOCALIZED_TEXT;
    case 22: return scada::Variant::EXTENSION_OBJECT;
    default: return scada::Variant::COUNT;
  }
}

void AppendExtensionObjectValue(Encoder& encoder,
                                const scada::ExtensionObject& value) {
  std::vector<char> body;
  if (const auto* raw_bytes = std::any_cast<scada::ByteString>(&value.value())) {
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

bool ReadExtensionObjectValue(Decoder& decoder, scada::ExtensionObject& value) {
  scada::ExpandedNodeId data_type_id;
  std::uint8_t encoding = 0;
  std::int32_t length = 0;
  if (!decoder.Decode(data_type_id) || !decoder.Decode(encoding)) {
    return false;
  }
  if (encoding == 0x00) {
    value = scada::ExtensionObject{std::move(data_type_id), scada::ByteString{}};
    return true;
  }
  if (!decoder.Decode(length) || length < 0 ||
      decoder.remaining().size() < static_cast<std::size_t>(length)) {
    return false;
  }
  scada::ByteString body(decoder.remaining().begin(),
                         decoder.remaining().begin() + length);
  decoder = Decoder{decoder.remaining().subspan(static_cast<std::size_t>(length))};
  value = scada::ExtensionObject{std::move(data_type_id), std::move(body)};
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

template <class T, class Reader>
bool ReadArray(Decoder& decoder,
               std::vector<T>& values,
               Reader&& reader) {
  std::int32_t count = 0;
  if (!decoder.Decode(count) || count < 0) {
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

void Encoder::Encode(const scada::String& value) {
  Encode(std::string_view{value});
}

void Encoder::Encode(const scada::QualifiedName& value) {
  Encode(value.namespace_index());
  Encode(value.name());
}

void Encoder::Encode(const scada::LocalizedText& value) {
  if (value.empty()) {
    Encode(std::uint8_t{0});
    return;
  }
  const auto utf8 = ToString(value);
  Encode(std::uint8_t{0x02});
  Encode(utf8);
}

void Encoder::Encode(scada::DateTime value) {
  if (value.is_null()) {
    Encode(std::int64_t{0});
    return;
  }
  Encode(value.ToDeltaSinceWindowsEpoch().InMicroseconds() * 10);
}

void Encoder::Encode(const scada::ByteString& value) {
  Encode(static_cast<std::int32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void Encoder::Encode(const scada::NodeId& node_id) {
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

void Encoder::Encode(const scada::ExpandedNodeId& node_id) {
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

void Encoder::Encode(const scada::Variant& value) {
  if (value.is_array()) {
    Encode(static_cast<std::uint8_t>(BuiltInTypeId(value.type()) | 0x80));
    switch (value.type()) {
      case scada::Variant::EMPTY:
        Encode(static_cast<std::int32_t>(
            value.get<std::vector<std::monostate>>().size()));
        return;
      case scada::Variant::BOOL:
        AppendArray(*this, value.get<std::vector<bool>>(),
                    [&](bool element) { Encode(element); });
        return;
      case scada::Variant::INT8:
        AppendArray(*this, value.get<std::vector<scada::Int8>>(),
                    [&](scada::Int8 element) {
                      Encode(static_cast<std::uint8_t>(element));
                    });
        return;
      case scada::Variant::UINT8:
        AppendArray(*this, value.get<std::vector<scada::UInt8>>(),
                    [&](scada::UInt8 element) { Encode(element); });
        return;
      case scada::Variant::INT16:
        AppendArray(*this, value.get<std::vector<scada::Int16>>(),
                    [&](scada::Int16 element) {
                      Encode(static_cast<std::uint16_t>(element));
                    });
        return;
      case scada::Variant::UINT16:
        AppendArray(*this, value.get<std::vector<scada::UInt16>>(),
                    [&](scada::UInt16 element) { Encode(element); });
        return;
      case scada::Variant::INT32:
        AppendArray(*this, value.get<std::vector<scada::Int32>>(),
                    [&](scada::Int32 element) { Encode(element); });
        return;
      case scada::Variant::UINT32:
        AppendArray(*this, value.get<std::vector<scada::UInt32>>(),
                    [&](scada::UInt32 element) { Encode(element); });
        return;
      case scada::Variant::INT64:
        AppendArray(*this, value.get<std::vector<scada::Int64>>(),
                    [&](scada::Int64 element) { Encode(element); });
        return;
      case scada::Variant::UINT64:
        AppendArray(*this, value.get<std::vector<scada::UInt64>>(),
                    [&](scada::UInt64 element) {
                      for (int i = 0; i < 8; ++i) {
                        Encode(static_cast<std::uint8_t>(
                            (element >> (8 * i)) & 0xff));
                      }
                    });
        return;
      case scada::Variant::DOUBLE:
        AppendArray(*this, value.get<std::vector<scada::Double>>(),
                    [&](scada::Double element) { Encode(element); });
        return;
      case scada::Variant::BYTE_STRING:
        AppendArray(*this, value.get<std::vector<scada::ByteString>>(),
                    [&](const scada::ByteString& element) { Encode(element); });
        return;
      case scada::Variant::STRING:
        AppendArray(*this, value.get<std::vector<scada::String>>(),
                    [&](const scada::String& element) {
                      Encode(element);
                    });
        return;
      case scada::Variant::QUALIFIED_NAME:
        AppendArray(*this, value.get<std::vector<scada::QualifiedName>>(),
                    [&](const scada::QualifiedName& element) { Encode(element); });
        return;
      case scada::Variant::LOCALIZED_TEXT:
        AppendArray(*this, value.get<std::vector<scada::LocalizedText>>(),
                    [&](const scada::LocalizedText& element) { Encode(element); });
        return;
      case scada::Variant::NODE_ID:
        AppendArray(*this, value.get<std::vector<scada::NodeId>>(),
                    [&](const scada::NodeId& element) { Encode(element); });
        return;
      case scada::Variant::EXPANDED_NODE_ID:
        AppendArray(*this, value.get<std::vector<scada::ExpandedNodeId>>(),
                    [&](const scada::ExpandedNodeId& element) { Encode(element); });
        return;
      case scada::Variant::EXTENSION_OBJECT:
        AppendArray(*this, value.get<std::vector<scada::ExtensionObject>>(),
                    [&](const scada::ExtensionObject& element) {
                      AppendExtensionObjectValue(*this, element);
                    });
        return;
      case scada::Variant::DATE_TIME:
      case scada::Variant::COUNT:
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
    case scada::Variant::EMPTY:
      return;
    case scada::Variant::BOOL:
      Encode(value.get<bool>());
      return;
    case scada::Variant::INT8:
      Encode(static_cast<std::uint8_t>(value.get<scada::Int8>()));
      return;
    case scada::Variant::UINT8:
      Encode(value.get<scada::UInt8>());
      return;
    case scada::Variant::INT16:
      Encode(static_cast<std::uint16_t>(value.get<scada::Int16>()));
      return;
    case scada::Variant::UINT16:
      Encode(value.get<scada::UInt16>());
      return;
    case scada::Variant::INT32:
      Encode(value.get<scada::Int32>());
      return;
    case scada::Variant::UINT32:
      Encode(value.get<scada::UInt32>());
      return;
    case scada::Variant::INT64:
      Encode(value.get<scada::Int64>());
      return;
    case scada::Variant::UINT64: {
      const auto raw = value.get<scada::UInt64>();
      for (int i = 0; i < 8; ++i) {
        Encode(static_cast<std::uint8_t>((raw >> (8 * i)) & 0xff));
      }
      return;
    }
    case scada::Variant::DOUBLE:
      Encode(value.get<scada::Double>());
      return;
    case scada::Variant::BYTE_STRING:
      Encode(value.get<scada::ByteString>());
      return;
    case scada::Variant::STRING:
      Encode(value.get<scada::String>());
      return;
    case scada::Variant::QUALIFIED_NAME:
      Encode(value.get<scada::QualifiedName>());
      return;
    case scada::Variant::LOCALIZED_TEXT:
      Encode(value.get<scada::LocalizedText>());
      return;
    case scada::Variant::NODE_ID:
      Encode(value.get<scada::NodeId>());
      return;
    case scada::Variant::EXPANDED_NODE_ID:
      Encode(value.get<scada::ExpandedNodeId>());
      return;
    case scada::Variant::EXTENSION_OBJECT:
      AppendExtensionObjectValue(*this, value.get<scada::ExtensionObject>());
      return;
    case scada::Variant::DATE_TIME:
      Encode(value.get<scada::DateTime>());
      return;
    case scada::Variant::COUNT:
      Encode(std::uint8_t{0});
      return;
  }
}

void Encoder::Encode(const EncodedExtensionObject& value) {
  Encode(scada::NodeId{value.type_id});
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

bool Decoder::Decode(scada::String& value) {
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

bool Decoder::Decode(scada::QualifiedName& value) {
  std::uint16_t namespace_index = 0;
  std::string name;
  if (!Decode(namespace_index) || !Decode(name)) {
    return false;
  }
  value = scada::QualifiedName{std::move(name), namespace_index};
  return true;
}

bool Decoder::Decode(scada::LocalizedText& value) {
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
  value = scada::ToLocalizedText(text);
  return true;
}

bool Decoder::Decode(scada::DateTime& value) {
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

bool Decoder::Decode(scada::ByteString& value) {
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

bool Decoder::Decode(scada::NodeId& id) {
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
    id = scada::NodeId{short_id, ns};
    return true;
  }
  if (encoding == 0x02) {
    std::uint16_t ns = 0;
    std::uint32_t numeric_id = 0;
    if (!Decode(ns) || !Decode(numeric_id)) {
      return false;
    }
    id = scada::NodeId{numeric_id, ns};
    return true;
  }
  if (encoding == 0x03) {
    std::uint16_t ns = 0;
    scada::String string_id;
    if (!Decode(ns) || !Decode(string_id)) {
      return false;
    }
    id = scada::NodeId{std::move(string_id), ns};
    return true;
  }
  if (encoding == 0x05) {
    std::uint16_t ns = 0;
    scada::ByteString opaque_id;
    if (!Decode(ns) || !Decode(opaque_id)) {
      return false;
    }
    id = scada::NodeId{std::move(opaque_id), ns};
    return true;
  }
  return false;
}

bool Decoder::Decode(scada::ExpandedNodeId& id) {
  std::uint8_t encoding = 0;
  if (!Decode(encoding)) {
    return false;
  }

  scada::NodeId node_id;
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
      node_id = scada::NodeId{short_id, ns};
      break;
    }
    case 0x02: {
      std::uint16_t ns = 0;
      std::uint32_t numeric_id = 0;
      if (!Decode(ns) || !Decode(numeric_id)) {
        return false;
      }
      node_id = scada::NodeId{numeric_id, ns};
      break;
    }
    case 0x03: {
      std::uint16_t ns = 0;
      scada::String string_id;
      if (!Decode(ns) || !Decode(string_id)) {
        return false;
      }
      node_id = scada::NodeId{std::move(string_id), ns};
      break;
    }
    case 0x05: {
      std::uint16_t ns = 0;
      scada::ByteString opaque_id;
      if (!Decode(ns) || !Decode(opaque_id)) {
        return false;
      }
      node_id = scada::NodeId{std::move(opaque_id), ns};
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

  id = scada::ExpandedNodeId{std::move(node_id), std::move(namespace_uri),
                             server_index};
  return true;
}

bool Decoder::Decode(scada::Variant& value) {
  std::uint8_t encoding_mask = 0;
  if (!Decode(encoding_mask) || (encoding_mask & 0x80) != 0) {
    if ((encoding_mask & 0x80) == 0) {
      return false;
    }
  }

  const bool is_array = (encoding_mask & 0x80) != 0;
  const auto type = FromBuiltInTypeId(encoding_mask & 0x3f);
  if (type == scada::Variant::COUNT) {
    return false;
  }

  if (is_array) {
    switch (type) {
      case scada::Variant::EMPTY: {
        std::int32_t count = 0;
        if (!Decode(count) || count < 0) {
          return false;
        }
        value = scada::Variant{std::vector<std::monostate>(
            static_cast<std::size_t>(count))};
        return true;
      }
      case scada::Variant::BOOL: {
        std::vector<bool> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](bool& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::INT8: {
        std::vector<scada::Int8> typed_values;
        if (!ReadArray(*this, typed_values, [&](scada::Int8& element) {
              std::uint8_t raw = 0;
              if (!Decode(raw)) {
                return false;
              }
              element = static_cast<scada::Int8>(raw);
              return true;
            })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::UINT8: {
        std::vector<scada::UInt8> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::UInt8& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::INT16: {
        std::vector<scada::Int16> typed_values;
        if (!ReadArray(*this, typed_values, [&](scada::Int16& element) {
              std::uint16_t raw = 0;
              if (!Decode(raw)) {
                return false;
              }
              element = static_cast<scada::Int16>(raw);
              return true;
            })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::UINT16: {
        std::vector<scada::UInt16> typed_values;
        if (!ReadArray(*this, typed_values, [&](scada::UInt16& element) {
              return Decode(element);
            })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::INT32: {
        std::vector<scada::Int32> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::Int32& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::UINT32: {
        std::vector<scada::UInt32> typed_values;
        if (!ReadArray(*this, typed_values, [&](scada::UInt32& element) {
              return Decode(element);
            })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::INT64: {
        std::vector<scada::Int64> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::Int64& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::UINT64: {
        std::vector<scada::UInt64> typed_values;
        if (!ReadArray(*this, typed_values, [&](scada::UInt64& element) {
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
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::DOUBLE: {
        std::vector<scada::Double> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::Double& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::BYTE_STRING: {
        std::vector<scada::ByteString> typed_values;
        if (!ReadArray(*this, typed_values, [&](scada::ByteString& element) {
              return Decode(element);
            })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::STRING: {
        std::vector<scada::String> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::String& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::QUALIFIED_NAME: {
        std::vector<scada::QualifiedName> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::QualifiedName& element) {
                         return Decode(element);
                       })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::LOCALIZED_TEXT: {
        std::vector<scada::LocalizedText> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::LocalizedText& element) {
                         return Decode(element);
                       })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::NODE_ID: {
        std::vector<scada::NodeId> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::NodeId& element) { return Decode(element); })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::EXPANDED_NODE_ID: {
        std::vector<scada::ExpandedNodeId> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::ExpandedNodeId& element) {
                         return Decode(element);
                       })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::EXTENSION_OBJECT: {
        std::vector<scada::ExtensionObject> typed_values;
        if (!ReadArray(*this, typed_values,
                       [&](scada::ExtensionObject& element) {
                         return ReadExtensionObjectValue(*this, element);
                       })) {
          return false;
        }
        value = scada::Variant{std::move(typed_values)};
        return true;
      }
      case scada::Variant::DATE_TIME:
      case scada::Variant::COUNT:
        return false;
    }
  }

  switch (type) {
    case scada::Variant::EMPTY:
      value = scada::Variant{};
      return true;
    case scada::Variant::BOOL: {
      bool typed_value = false;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::INT8: {
      std::uint8_t raw = 0;
      if (!Decode(raw)) return false;
      value = scada::Variant{static_cast<scada::Int8>(raw)};
      return true;
    }
    case scada::Variant::UINT8: {
      scada::UInt8 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::INT16: {
      std::uint16_t raw = 0;
      if (!Decode(raw)) return false;
      value = scada::Variant{static_cast<scada::Int16>(raw)};
      return true;
    }
    case scada::Variant::UINT16: {
      scada::UInt16 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::INT32: {
      scada::Int32 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::UINT32: {
      scada::UInt32 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::INT64: {
      scada::Int64 typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::UINT64: {
      scada::UInt64 typed_value = 0;
      for (int i = 0; i < 8; ++i) {
        std::uint8_t byte = 0;
        if (!Decode(byte)) return false;
        typed_value |= static_cast<std::uint64_t>(byte) << (8 * i);
      }
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::DOUBLE: {
      double typed_value = 0;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::BYTE_STRING: {
      scada::ByteString typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::STRING: {
      std::string typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::QUALIFIED_NAME: {
      scada::QualifiedName typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::LOCALIZED_TEXT: {
      scada::LocalizedText typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::NODE_ID: {
      scada::NodeId typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::EXPANDED_NODE_ID: {
      scada::ExpandedNodeId typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::EXTENSION_OBJECT: {
      scada::ExtensionObject typed_value;
      if (!ReadExtensionObjectValue(*this, typed_value)) return false;
      value = scada::Variant{std::move(typed_value)};
      return true;
    }
    case scada::Variant::DATE_TIME: {
      scada::DateTime typed_value;
      if (!Decode(typed_value)) return false;
      value = scada::Variant{typed_value};
      return true;
    }
    case scada::Variant::COUNT:
      return false;
  }

  return false;
}

bool Decoder::Decode(DecodedExtensionObject& value) {
  scada::NodeId node_id;
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
  encoder.Encode(scada::NodeId{type_id});
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(static_cast<std::int32_t>(payload.size()));
  encoder.bytes().insert(encoder.bytes().end(), payload.begin(), payload.end());
}

std::optional<std::pair<std::uint32_t, std::span<const char>>> ReadMessage(
    Decoder& decoder) {
  scada::NodeId type_id;
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
