#include "opcua/ua/ua_json_builtins.h"

#include "opcua/base/base64.h"
#include "opcua/base/time_utils.h"
#include "opcua/base/utf_convert.h"

#include <boost/json.hpp>

#include <charconv>
#include <format>
#include <limits>

namespace opcua::ua::json {
namespace {

using boost::json::array;
using boost::json::object;
using boost::json::string;
using boost::json::value;

const object& RequireObject(const value& json) {
  if (!json.is_object())
    ThrowError("expected a JSON object");
  return json.as_object();
}

const array& RequireArray(const value& json) {
  if (!json.is_array())
    ThrowError("expected a JSON array");
  return json.as_array();
}

std::string_view RequireString(const value& json) {
  if (!json.is_string())
    ThrowError("expected a JSON string");
  return json.as_string();
}

std::int64_t RequireInt64(const value& json) {
  if (json.is_int64())
    return json.as_int64();
  if (json.is_uint64() &&
      json.as_uint64() <= static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
    return static_cast<std::int64_t>(json.as_uint64());
  }
  ThrowError("expected a JSON integer");
}

std::uint64_t RequireUInt64(const value& json) {
  if (json.is_uint64())
    return json.as_uint64();
  if (json.is_int64() && json.as_int64() >= 0)
    return static_cast<std::uint64_t>(json.as_int64());
  ThrowError("expected a JSON unsigned integer");
}

double RequireDouble(const value& json) {
  if (json.is_double())
    return json.as_double();
  if (json.is_int64())
    return static_cast<double>(json.as_int64());
  if (json.is_uint64())
    return static_cast<double>(json.as_uint64());
  ThrowError("expected a JSON number");
}

// Narrows a decoded integer, rejecting values the target cannot hold rather
// than silently truncating them.
template <class T>
T NarrowSigned(const value& json) {
  const std::int64_t raw = RequireInt64(json);
  if (raw < std::numeric_limits<T>::min() ||
      raw > std::numeric_limits<T>::max())
    ThrowError("integer out of range");
  return static_cast<T>(raw);
}

template <class T>
T NarrowUnsigned(const value& json) {
  const std::uint64_t raw = RequireUInt64(json);
  if (raw > std::numeric_limits<T>::max())
    ThrowError("integer out of range");
  return static_cast<T>(raw);
}

}  // namespace

void ThrowError(std::string_view message) {
  throw Error{message};
}

const value& RequireField(const object& json, std::string_view name) {
  const value* field = json.if_contains(name);
  if (field == nullptr)
    ThrowError(std::string{"missing field "} + std::string{name});
  return *field;
}

// -- Numbers and strings ----------------------------------------------------

value Encode(Boolean value) {
  return value;
}
value Encode(Int8 value) {
  return static_cast<std::int64_t>(value);
}
value Encode(UInt8 value) {
  return static_cast<std::uint64_t>(value);
}
value Encode(Int16 value) {
  return static_cast<std::int64_t>(value);
}
value Encode(UInt16 value) {
  return static_cast<std::uint64_t>(value);
}
value Encode(Int32 value) {
  return static_cast<std::int64_t>(value);
}
value Encode(UInt32 value) {
  return static_cast<std::uint64_t>(value);
}
value Encode(Int64 value) {
  // OPC UA Part 6 §5.4.2.3: Int64 and UInt64 are encoded as JSON *strings*,
  // because JSON numbers cannot carry 64 bits without loss,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.3
  return string(std::to_string(value));
}
value Encode(UInt64 value) {
  return string(std::to_string(value));
}
value Encode(Float value) {
  return static_cast<double>(value);
}
value Encode(Double value) {
  return value;
}
value Encode(const String& value) {
  return string(value);
}

void Decode(const value& json, Boolean& value) {
  if (!json.is_bool())
    ThrowError("expected a JSON boolean");
  value = json.as_bool();
}
void Decode(const value& json, Int8& value) {
  value = NarrowSigned<Int8>(json);
}
void Decode(const value& json, UInt8& value) {
  value = NarrowUnsigned<UInt8>(json);
}
void Decode(const value& json, Int16& value) {
  value = NarrowSigned<Int16>(json);
}
void Decode(const value& json, UInt16& value) {
  value = NarrowUnsigned<UInt16>(json);
}
void Decode(const value& json, Int32& value) {
  value = NarrowSigned<Int32>(json);
}
void Decode(const value& json, UInt32& value) {
  value = NarrowUnsigned<UInt32>(json);
}

void Decode(const value& json, Int64& value) {
  // Accepts the spec's string form and, for tolerance, a plain JSON number.
  if (json.is_string()) {
    const std::string_view text = json.as_string();
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
      ThrowError("malformed Int64");
    return;
  }
  value = RequireInt64(json);
}

void Decode(const value& json, UInt64& value) {
  if (json.is_string()) {
    const std::string_view text = json.as_string();
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
      ThrowError("malformed UInt64");
    return;
  }
  value = RequireUInt64(json);
}

void Decode(const value& json, Float& value) {
  value = static_cast<Float>(RequireDouble(json));
}
void Decode(const value& json, Double& value) {
  value = RequireDouble(json);
}
void Decode(const value& json, String& value) {
  // A null String is JSON null, which decodes to the empty string: opcuapp's
  // String cannot represent the null/empty distinction.
  if (json.is_null()) {
    value.clear();
    return;
  }
  value = std::string{RequireString(json)};
}

// -- Temporal, binary and identifier types ----------------------------------

value Encode(DateTime value) {
  // OPC UA Part 6 §5.4.2.6 DateTime: ISO 8601 UTC,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.6
  if (value.is_null())
    return "0001-01-01T00:00:00Z";
  DateTime::Exploded exploded = {};
  value.UTCExplode(&exploded);
  auto text = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", exploded.year,
                          exploded.month, exploded.day_of_month, exploded.hour,
                          exploded.minute, exploded.second);
  if (exploded.millisecond != 0)
    text += std::format(".{:03}", exploded.millisecond);
  text += 'Z';
  return string(std::move(text));
}

void Decode(const value& json, DateTime& value) {
  if (json.is_null()) {
    value = DateTime{};
    return;
  }
  const std::string_view text = RequireString(json);
  if (text == "0001-01-01T00:00:00Z") {
    value = DateTime{};
    return;
  }
  if (!Deserialize(text, value))
    ThrowError("malformed DateTime");
}

value Encode(const Guid& value) {
  // OPC UA Part 6 §5.4.2.5 Guid: the canonical text form.
  return string(value.ToString());
}

void Decode(const value& json, Guid& value) {
  const std::optional<Guid> guid = Guid::FromString(RequireString(json));
  if (!guid.has_value())
    ThrowError("malformed Guid");
  value = *guid;
}

value Encode(const ByteString& value) {
  // OPC UA Part 6 §5.4.2.7 ByteString: base64. (The pre-generation websocket
  // codec emitted an array of byte values instead, which no conforming peer
  // accepts.)
  std::string encoded;
  opcua::base::Base64Encode(std::string_view{value.data(), value.size()},
                            &encoded);
  return string(std::move(encoded));
}

void Decode(const value& json, ByteString& value) {
  if (json.is_null()) {
    value.clear();
    return;
  }
  std::string decoded;
  if (!opcua::base::Base64Decode(RequireString(json), &decoded))
    ThrowError("malformed ByteString");
  value.assign(decoded.begin(), decoded.end());
}

value Encode(const XmlElement& value) {
  // OPC UA Part 6 §5.4.2.8 XmlElement: the XML text as a JSON string.
  return string(value.value);
}

void Decode(const value& json, XmlElement& value) {
  if (json.is_null()) {
    value.value.clear();
    return;
  }
  value.value = std::string{RequireString(json)};
}

value Encode(const NodeId& value) {
  // OPC UA Part 6 §5.4.2.10 NodeId: the text form of Part 3 §8.2 ("ns=2;i=5"),
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.10
  return string(value.ToString());
}

void Decode(const value& json, NodeId& value) {
  const std::string_view text = RequireString(json);
  value = NodeId::FromString(text);
  if (value.is_null() && text != "i=0")
    ThrowError("malformed NodeId");
}

value Encode(const ExpandedNodeId& value) {
  // OPC UA Part 6 §5.4.2.11 ExpandedNodeId: the NodeId text optionally
  // prefixed with `svr=<index>;` and/or `nsu=<uri>;`.
  std::string text;
  if (value.server_index() != 0)
    text += "svr=" + std::to_string(value.server_index()) + ";";
  if (!value.namespace_uri().empty())
    text += "nsu=" + value.namespace_uri() + ";";
  text += value.node_id().ToString();
  return string(std::move(text));
}

void Decode(const value& json, ExpandedNodeId& value) {
  std::string_view text = RequireString(json);
  unsigned server_index = 0;
  std::string namespace_uri;
  while (true) {
    if (text.starts_with("svr=")) {
      const auto separator = text.find(';', 4);
      if (separator == std::string_view::npos)
        ThrowError("malformed ExpandedNodeId: no ; after svr=");
      server_index = static_cast<unsigned>(
          std::stoul(std::string{text.substr(4, separator - 4)}));
      text.remove_prefix(separator + 1);
      continue;
    }
    if (text.starts_with("nsu=")) {
      const auto separator = text.find(';', 4);
      if (separator == std::string_view::npos)
        ThrowError("malformed ExpandedNodeId: no ; after nsu=");
      namespace_uri = std::string{text.substr(4, separator - 4)};
      text.remove_prefix(separator + 1);
      continue;
    }
    break;
  }
  NodeId node_id = NodeId::FromString(text);
  if (node_id.is_null() && text != "i=0")
    ThrowError("malformed ExpandedNodeId");
  value = ExpandedNodeId{std::move(node_id), std::move(namespace_uri),
                         server_index};
}

value Encode(Status value) {
  // OPC UA Part 6 §5.4.2.12 StatusCode: an object with the numeric `Code` and
  // an optional human-readable `Symbol`. Good is the default and its Code is
  // omitted. (The pre-generation websocket codec sent a bare number.)
  object json;
  if (value.full_code() != 0) {
    json["Code"] = static_cast<std::uint64_t>(value.full_code());
    json["Symbol"] = ToString(value.code());
  }
  return json;
}

void Decode(const value& json, Status& value) {
  // Tolerates a bare number as well as the object form.
  if (json.is_int64() || json.is_uint64()) {
    value = Status::FromFullCode(static_cast<unsigned>(RequireUInt64(json)));
    return;
  }
  const object& obj = RequireObject(json);
  const boost::json::value* code = obj.if_contains("Code");
  value =
      code == nullptr
          ? Status{StatusCode::Good}
          : Status::FromFullCode(static_cast<unsigned>(RequireUInt64(*code)));
}

value Encode(const QualifiedName& value) {
  // OPC UA Part 6 §5.4.2.14 QualifiedName: the text form "<ns>:<name>", with
  // the namespace index omitted when zero,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.14
  if (value.namespace_index() == 0)
    return string(value.name());
  return string(std::to_string(value.namespace_index()) + ":" + value.name());
}

void Decode(const value& json, QualifiedName& value) {
  if (json.is_null()) {
    value = QualifiedName{};
    return;
  }
  // The object form of earlier releases is still accepted so a peer that has
  // not been updated keeps working.
  if (json.is_object()) {
    const object& obj = json.as_object();
    NamespaceIndex namespace_index = 0;
    if (const boost::json::value* uri = obj.if_contains("Uri"))
      namespace_index = static_cast<NamespaceIndex>(RequireUInt64(*uri));
    value = QualifiedName{std::string{RequireString(RequireField(obj, "Name"))},
                          namespace_index};
    return;
  }
  const std::string_view text = RequireString(json);
  const auto separator = text.find(':');
  if (separator == std::string_view::npos) {
    value = QualifiedName{std::string{text}, 0};
    return;
  }
  unsigned namespace_index = 0;
  const std::string_view prefix = text.substr(0, separator);
  const auto result = std::from_chars(
      prefix.data(), prefix.data() + prefix.size(), namespace_index);
  if (result.ec != std::errc{} || result.ptr != prefix.data() + prefix.size()) {
    // A name containing a colon but no numeric prefix is a plain name.
    value = QualifiedName{std::string{text}, 0};
    return;
  }
  value = QualifiedName{std::string{text.substr(separator + 1)},
                        static_cast<NamespaceIndex>(namespace_index)};
}

value Encode(const LocalizedText& value) {
  // OPC UA Part 6 §5.4.2.15 LocalizedText: `{Locale, Text}`, each omitted when
  // empty.
  object json;
  if (!value.locale.empty())
    json["Locale"] = value.locale;
  std::string text = UtfConvert<char>(value.text);
  if (!text.empty())
    json["Text"] = std::move(text);
  return json;
}

void Decode(const value& json, LocalizedText& value) {
  if (json.is_null()) {
    value = LocalizedText{};
    return;
  }
  const object& obj = RequireObject(json);
  value = LocalizedText{};
  if (const boost::json::value* locale = obj.if_contains("Locale"))
    value.locale = std::string{RequireString(*locale)};
  if (const boost::json::value* text = obj.if_contains("Text"))
    value.text = UtfConvert<char16_t>(std::string{RequireString(*text)});
}

value Encode(const ExtensionObject& value) {
  // OPC UA Part 6 §5.4.2.16 ExtensionObject: `{UaTypeId, UaEncoding, UaBody}`,
  // where UaEncoding 1 means the body is a base64 ByteString holding the
  // Binary encoding. A body this stack could not decode is passed through
  // verbatim.
  object json;
  json["UaTypeId"] = Encode(value.data_type_id());
  if (const ByteString* body = value.binary_body()) {
    json["UaEncoding"] = 1;
    json["UaBody"] = Encode(*body);
  } else if (const auto* body =
                 std::any_cast<boost::json::value>(&value.value())) {
    json["UaBody"] = *body;
  }
  return json;
}

void Decode(const value& json, ExtensionObject& value) {
  const object& obj = RequireObject(json);
  ExpandedNodeId data_type_id;
  Decode(RequireField(obj, "UaTypeId"), data_type_id);
  const boost::json::value* body = obj.if_contains("UaBody");
  if (body == nullptr) {
    value = ExtensionObject{std::move(data_type_id), std::any{}};
    return;
  }
  const boost::json::value* encoding = obj.if_contains("UaEncoding");
  if (encoding != nullptr && RequireUInt64(*encoding) == 1) {
    ByteString bytes;
    Decode(*body, bytes);
    value = ExtensionObject{std::move(data_type_id), std::move(bytes)};
    return;
  }
  value = ExtensionObject{std::move(data_type_id), *body};
}

// -- Variant, DataValue and DiagnosticInfo ----------------------------------

namespace {

// The Variant payload types, as (enumerator, scalar alternative, array
// element) — the same shape as the binary codec's list in codec_utils.cpp, and
// for the same reason: encode and decode, scalar and array, all expand from
// one place. VARIANT and DATA_VALUE nest behind a shared pointer because both
// are recursive through Variant.
#define OPCUA_JSON_VARIANT_TYPES(V)                     \
  V(BOOL, Boolean, Boolean)                             \
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
  V(DIAGNOSTIC_INFO, DiagnosticInfo, DiagnosticInfo)

template <class T>
value EncodeScalarList(const std::vector<T>& values) {
  array result;
  result.reserve(values.size());
  for (const T& item : values)
    result.emplace_back(Encode(item));
  return result;
}

template <class T>
std::vector<T> DecodeScalarList(const value& json) {
  std::vector<T> values;
  const array& source = RequireArray(json);
  values.resize(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    // Indexed rather than range-based: std::vector<bool> hands out a proxy.
    T element{};
    Decode(source[index], element);
    values[index] = std::move(element);
  }
  return values;
}

// Encodes a Variant's payload — the `Value` member — without the surrounding
// UaType. Arrays become JSON arrays of the same.
value EncodeVariantPayload(const Variant& variant) {
  switch (variant.type()) {
    case Variant::EMPTY:
      return nullptr;
#define OPCUA_ENCODE_VARIANT_PAYLOAD(NAME, SCALAR, ELEMENT)            \
  case Variant::NAME:                                                  \
    return variant.is_array()                                          \
               ? EncodeScalarList(variant.get<std::vector<ELEMENT>>()) \
               : Encode(variant.get<SCALAR>());
      OPCUA_JSON_VARIANT_TYPES(OPCUA_ENCODE_VARIANT_PAYLOAD)
#undef OPCUA_ENCODE_VARIANT_PAYLOAD
    // Nested DataValue and Variant travel behind a shared pointer.
    case Variant::DATA_VALUE: {
      const auto& nested = variant.get<std::shared_ptr<const DataValue>>();
      return Encode(nested ? *nested : DataValue{});
    }
    case Variant::VARIANT: {
      const auto& nested = variant.get<std::shared_ptr<const Variant>>();
      return Encode(nested ? *nested : Variant{});
    }
    case Variant::COUNT:
      break;
  }
  ThrowError("unsupported Variant type");
}

void DecodeVariantPayload(Variant::Type type,
                          const value& json,
                          Variant& variant) {
  const bool is_array = json.is_array();
  switch (type) {
    case Variant::EMPTY:
      variant = Variant{};
      return;
#define OPCUA_DECODE_VARIANT_PAYLOAD(NAME, SCALAR, ELEMENT) \
  case Variant::NAME: {                                     \
    if (is_array) {                                         \
      variant = Variant{DecodeScalarList<ELEMENT>(json)};   \
      return;                                               \
    }                                                       \
    SCALAR element{};                                       \
    Decode(json, element);                                  \
    variant = Variant{std::move(element)};                  \
    return;                                                 \
  }
      OPCUA_JSON_VARIANT_TYPES(OPCUA_DECODE_VARIANT_PAYLOAD)
#undef OPCUA_DECODE_VARIANT_PAYLOAD
    case Variant::DATA_VALUE: {
      DataValue nested;
      Decode(json, nested);
      variant = Variant{std::make_shared<const DataValue>(std::move(nested))};
      return;
    }
    case Variant::VARIANT: {
      Variant nested;
      Decode(json, nested);
      variant = Variant{std::make_shared<const Variant>(std::move(nested))};
      return;
    }
    case Variant::COUNT:
      break;
  }
  ThrowError("unsupported Variant type");
}

}  // namespace

value Encode(const Variant& value) {
  // OPC UA Part 6 §5.4.2.17 Variant: `{UaType, Value, Dimensions}`, where
  // UaType is the BuiltInType id. A null Variant is JSON null.
  if (value.type() == Variant::EMPTY && !value.is_array())
    return nullptr;
  object json;
  json["UaType"] = static_cast<std::uint64_t>(value.type());
  json["Value"] = EncodeVariantPayload(value);
  return json;
}

void Decode(const value& json, Variant& value) {
  if (json.is_null()) {
    value = Variant{};
    return;
  }
  const object& obj = RequireObject(json);
  const boost::json::value* type = obj.if_contains("UaType");
  if (type == nullptr) {
    value = Variant{};
    return;
  }
  const auto type_id = static_cast<unsigned>(RequireUInt64(*type));
  if (type_id >= static_cast<unsigned>(Variant::COUNT))
    ThrowError("unsupported Variant UaType");
  const boost::json::value* payload = obj.if_contains("Value");
  if (payload == nullptr || payload->is_null()) {
    value = Variant{};
    return;
  }
  DecodeVariantPayload(static_cast<Variant::Type>(type_id), *payload, value);
}

value Encode(const DataValue& value) {
  // OPC UA Part 6 §5.4.2.18 DataValue. The Variant is *inlined*: UaType,
  // Value and Dimensions are DataValue's own members rather than a nested
  // Variant object.
  object json;
  if (!value.value.is_null()) {
    json["UaType"] = static_cast<std::uint64_t>(value.value.type());
    json["Value"] = EncodeVariantPayload(value.value);
  }
  if (!IsGood(value.status_code))
    json["StatusCode"] = Encode(Status{value.status_code});
  if (!value.source_timestamp.is_null())
    json["SourceTimestamp"] = Encode(value.source_timestamp);
  if (!value.server_timestamp.is_null())
    json["ServerTimestamp"] = Encode(value.server_timestamp);
  return json;
}

void Decode(const value& json, DataValue& value) {
  const object& obj = RequireObject(json);
  value = DataValue{};
  if (const boost::json::value* type = obj.if_contains("UaType")) {
    const auto type_id = static_cast<unsigned>(RequireUInt64(*type));
    if (type_id >= static_cast<unsigned>(Variant::COUNT))
      ThrowError("unsupported DataValue UaType");
    if (const boost::json::value* payload = obj.if_contains("Value")) {
      if (!payload->is_null()) {
        DecodeVariantPayload(static_cast<Variant::Type>(type_id), *payload,
                             value.value);
      }
    }
  }
  if (const boost::json::value* status = obj.if_contains("StatusCode")) {
    Status decoded{StatusCode::Good};
    Decode(*status, decoded);
    value.status_code = decoded.code();
  }
  if (const boost::json::value* timestamp = obj.if_contains("SourceTimestamp"))
    Decode(*timestamp, value.source_timestamp);
  if (const boost::json::value* timestamp = obj.if_contains("ServerTimestamp"))
    Decode(*timestamp, value.server_timestamp);
}

value Encode(const DiagnosticInfo& value) {
  // OPC UA Part 6 §5.4.2.13 DiagnosticInfo. The namespace field is spelled
  // `NamespaceUri` here, unlike the binary dictionary's `NamespaceURI`.
  object json;
  if (value.symbolic_id.has_value())
    json["SymbolicId"] = *value.symbolic_id;
  if (value.namespace_uri.has_value())
    json["NamespaceUri"] = *value.namespace_uri;
  if (value.locale.has_value())
    json["Locale"] = *value.locale;
  if (value.localized_text.has_value())
    json["LocalizedText"] = *value.localized_text;
  if (value.additional_info.has_value())
    json["AdditionalInfo"] = *value.additional_info;
  if (value.inner_status_code.has_value())
    json["InnerStatusCode"] = Encode(*value.inner_status_code);
  if (value.inner_diagnostic_info != nullptr)
    json["InnerDiagnosticInfo"] = Encode(*value.inner_diagnostic_info);
  return json;
}

void Decode(const value& json, DiagnosticInfo& value) {
  const object& obj = RequireObject(json);
  value = DiagnosticInfo{};
  if (const boost::json::value* field = obj.if_contains("SymbolicId"))
    value.symbolic_id = static_cast<Int32>(RequireInt64(*field));
  if (const boost::json::value* field = obj.if_contains("NamespaceUri"))
    value.namespace_uri = static_cast<Int32>(RequireInt64(*field));
  if (const boost::json::value* field = obj.if_contains("Locale"))
    value.locale = static_cast<Int32>(RequireInt64(*field));
  if (const boost::json::value* field = obj.if_contains("LocalizedText"))
    value.localized_text = static_cast<Int32>(RequireInt64(*field));
  if (const boost::json::value* field = obj.if_contains("AdditionalInfo"))
    value.additional_info = std::string{RequireString(*field)};
  if (const boost::json::value* field = obj.if_contains("InnerStatusCode")) {
    Status inner{StatusCode::Good};
    Decode(*field, inner);
    value.inner_status_code = inner;
  }
  if (const boost::json::value* field =
          obj.if_contains("InnerDiagnosticInfo")) {
    DiagnosticInfo inner;
    Decode(*field, inner);
    value.inner_diagnostic_info =
        std::make_shared<const DiagnosticInfo>(std::move(inner));
  }
}

// -- Default detection ------------------------------------------------------

bool IsDefault(Boolean value) {
  return !value;
}
bool IsDefault(Int8 value) {
  return value == 0;
}
bool IsDefault(UInt8 value) {
  return value == 0;
}
bool IsDefault(Int16 value) {
  return value == 0;
}
bool IsDefault(UInt16 value) {
  return value == 0;
}
bool IsDefault(Int32 value) {
  return value == 0;
}
bool IsDefault(UInt32 value) {
  return value == 0;
}
bool IsDefault(Int64 value) {
  return value == 0;
}
bool IsDefault(UInt64 value) {
  return value == 0;
}
bool IsDefault(Float value) {
  return value == 0;
}
bool IsDefault(Double value) {
  return value == 0;
}
bool IsDefault(const String& value) {
  return value.empty();
}
bool IsDefault(DateTime value) {
  return value.is_null();
}
bool IsDefault(const Guid& value) {
  return value.is_null();
}
bool IsDefault(const ByteString& value) {
  return value.empty();
}
bool IsDefault(const XmlElement& value) {
  return value.empty();
}
bool IsDefault(const NodeId& value) {
  return value.is_null();
}
bool IsDefault(const ExpandedNodeId& value) {
  return value.node_id().is_null() && value.namespace_uri().empty() &&
         value.server_index() == 0;
}
bool IsDefault(Status value) {
  return value.full_code() == 0;
}
bool IsDefault(const QualifiedName& value) {
  return value.empty();
}
bool IsDefault(const LocalizedText& value) {
  return value.empty();
}
bool IsDefault(const ExtensionObject& value) {
  return value.data_type_id().node_id().is_null() && !value.has_value();
}
bool IsDefault(const Variant& value) {
  return value.is_null() && !value.is_array();
}
bool IsDefault(const DataValue& value) {
  return value.is_null() && IsGood(value.status_code) &&
         value.source_timestamp.is_null() && value.server_timestamp.is_null();
}
bool IsDefault(const DiagnosticInfo& value) {
  return value.empty();
}

}  // namespace opcua::ua::json
