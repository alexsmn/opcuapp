#include "opcua/transport/websocket/json_codec.h"

#include "opcua/base/time_utils.h"
#include "opcua/base/utf_convert.h"
#include "opcua/services/history_conversion.h"
#include "opcua/types/standard_node_ids.h"
#include "opcua/types/status.h"
#include "opcua/types/variant.h"
#include "opcua/ua/ua_json_codec.h"

#include <boost/json.hpp>

#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace opcua::ws {

namespace {

using boost::json::array;
using boost::json::object;
using boost::json::string;
using boost::json::value;

[[noreturn]] void ThrowJsonError(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

const object& RequireObject(const value& json) {
  if (!json.is_object())
    ThrowJsonError("Expected JSON object");
  return json.as_object();
}

const array& RequireArray(const value& json) {
  if (!json.is_array())
    ThrowJsonError("Expected JSON array");
  return json.as_array();
}

std::string_view RequireString(const value& json) {
  if (!json.is_string())
    ThrowJsonError("Expected JSON string");
  return json.as_string();
}

bool RequireBool(const value& json) {
  if (!json.is_bool())
    ThrowJsonError("Expected JSON bool");
  return json.as_bool();
}

std::int64_t RequireInt64(const value& json) {
  if (json.is_int64())
    return json.as_int64();
  if (json.is_uint64() &&
      json.as_uint64() <= static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
    return static_cast<std::int64_t>(json.as_uint64());
  }
  ThrowJsonError("Expected JSON integer");
}

std::uint64_t RequireUInt64(const value& json) {
  if (json.is_uint64())
    return json.as_uint64();
  if (json.is_int64() && json.as_int64() >= 0)
    return static_cast<std::uint64_t>(json.as_int64());
  ThrowJsonError("Expected JSON unsigned integer");
}

double RequireDouble(const value& json) {
  if (json.is_double())
    return json.as_double();
  if (json.is_int64())
    return static_cast<double>(json.as_int64());
  if (json.is_uint64())
    return static_cast<double>(json.as_uint64());
  ThrowJsonError("Expected JSON number");
}

const value& RequireField(const object& json, std::string_view key) {
  if (const auto* field = json.if_contains(key))
    return *field;
  ThrowJsonError("Missing required field");
}

const value* FindField(const object& json, std::string_view key) {
  return json.if_contains(key);
}

value EncodeNodeId(const NodeId& node_id) {
  return string(node_id.ToString());
}

NodeId DecodeNodeId(const value& json) {
  auto node_id = NodeId::FromString(RequireString(json));
  if (node_id.is_null() && RequireString(json) != "i=0")
    ThrowJsonError("Invalid NodeId");
  return node_id;
}

// OPC UA Part 6 §5.4.2.11: ExpandedNodeId is encoded as a JSON string using
// the textual form from §5.1.13 — the same NodeId text optionally prefixed
// with `nsu=<URI>;` (when NamespaceUri is set) and/or `svr=<index>;` (when
// ServerIndex is non-zero).
value EncodeExpandedNodeId(const ExpandedNodeId& node_id) {
  std::string text;
  if (node_id.server_index() != 0) {
    text += "svr=";
    text += std::to_string(node_id.server_index());
    text += ';';
  }
  if (!node_id.namespace_uri().empty()) {
    text += "nsu=";
    text += node_id.namespace_uri();
    text += ';';
  }
  text += node_id.node_id().ToString();
  return string(std::move(text));
}

ExpandedNodeId DecodeExpandedNodeId(const value& json) {
  std::string_view text = RequireString(json);
  unsigned server_index = 0;
  std::string namespace_uri;
  // Strip leading svr= and nsu= prefixes in any order.
  while (true) {
    if (text.starts_with("svr=")) {
      auto sep = text.find(';', 4);
      if (sep == std::string_view::npos)
        ThrowJsonError("Malformed ExpandedNodeId: missing ; after svr=");
      server_index = static_cast<unsigned>(
          std::stoul(std::string{text.substr(4, sep - 4)}));
      text.remove_prefix(sep + 1);
      continue;
    }
    if (text.starts_with("nsu=")) {
      auto sep = text.find(';', 4);
      if (sep == std::string_view::npos)
        ThrowJsonError("Malformed ExpandedNodeId: missing ; after nsu=");
      namespace_uri = std::string{text.substr(4, sep - 4)};
      text.remove_prefix(sep + 1);
      continue;
    }
    break;
  }
  auto node_id = NodeId::FromString(text);
  if (node_id.is_null() && text != "i=0")
    ThrowJsonError("Invalid ExpandedNodeId");
  return {std::move(node_id), std::move(namespace_uri), server_index};
}

// OPC UA Part 6 §5.4.2.13: QualifiedName is an object with `Name` (string)
// and `Uri` (NamespaceIndex as a number; the spec name is `Uri` even though
// it carries an integer index). `Uri` is omitted when NamespaceIndex == 0.
value EncodeQualifiedName(const QualifiedName& name) {
  object json{{"Name", name.name()}};
  if (name.namespace_index() != 0)
    json["Uri"] = name.namespace_index();
  return json;
}

QualifiedName DecodeQualifiedName(const value& json) {
  const auto& obj = RequireObject(json);
  NamespaceIndex namespace_index = 0;
  if (const auto* uri = FindField(obj, "Uri"))
    namespace_index = static_cast<NamespaceIndex>(RequireUInt64(*uri));
  return {std::string{RequireString(RequireField(obj, "Name"))},
          namespace_index};
}

// OPC UA Part 6 §5.4.2.14: LocalizedText is an object with optional `Locale`
// and `Text` string fields, both omitted when null/empty,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.14
value EncodeLocalizedText(const LocalizedText& text) {
  object json;
  if (!text.locale.empty())
    json["Locale"] = text.locale;
  std::string utf8 = UtfConvert<char>(text.text);
  if (!utf8.empty())
    json["Text"] = std::move(utf8);
  return json;
}

LocalizedText DecodeLocalizedText(const value& json) {
  const auto& obj = RequireObject(json);
  LocalizedText result;
  if (const auto* locale = FindField(obj, "Locale"))
    result.locale = std::string{RequireString(*locale)};
  if (const auto* text = FindField(obj, "Text"))
    result.text = UtfConvert<char16_t>(std::string{RequireString(*text)});
  return result;
}

value EncodeDateTime(DateTime time) {
  if (time.is_null())
    return nullptr;
  DateTime::Exploded e = {};
  time.UTCExplode(&e);
  auto text = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", e.year,
                          e.month, e.day_of_month, e.hour, e.minute, e.second);
  if (e.millisecond != 0)
    text += std::format(".{:03}", e.millisecond);
  text += 'Z';
  return string(std::move(text));
}

DateTime DecodeDateTime(const value& json) {
  if (json.is_null())
    return {};
  if (RequireString(json) == "0001-01-01T00:00:00Z")
    return {};
  DateTime time;
  if (!Deserialize(RequireString(json), time))
    ThrowJsonError("Invalid date-time");
  return time;
}

value EncodeByteString(const ByteString& bytes) {
  array result;
  result.reserve(bytes.size());
  for (char byte : bytes)
    result.emplace_back(
        static_cast<std::uint64_t>(static_cast<unsigned char>(byte)));
  return result;
}

ByteString DecodeByteString(const value& json) {
  ByteString bytes;
  for (const auto& entry : RequireArray(json)) {
    auto raw = RequireUInt64(entry);
    if (raw > std::numeric_limits<unsigned char>::max())
      ThrowJsonError("ByteString element out of range");
    bytes.push_back(static_cast<char>(static_cast<unsigned char>(raw)));
  }
  return bytes;
}

value EncodeExtensionObject(const ExtensionObject& extension_object) {
  const auto* payload =
      std::any_cast<boost::json::value>(&extension_object.value());
  if (!payload)
    ThrowJsonError("Unsupported ExtensionObject payload");
  return object{
      {"TypeId", EncodeExpandedNodeId(extension_object.data_type_id())},
      {"Body", *payload}};
}

ExtensionObject DecodeExtensionObject(const value& json) {
  const auto& obj = RequireObject(json);
  return {DecodeExpandedNodeId(RequireField(obj, "TypeId")),
          RequireField(obj, "Body")};
}

value EncodeStatus(const Status& status) {
  return static_cast<std::uint64_t>(status.full_code());
}

Status DecodeStatus(const value& json) {
  if (json.is_uint64() || (json.is_int64() && json.as_int64() >= 0)) {
    return Status::FromFullCode(static_cast<unsigned>(RequireUInt64(json)));
  }
  const auto& obj = RequireObject(json);
  return Status::FromFullCode(
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "fullCode"))));
}

value EncodeStatusCode(StatusCode status_code) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(status_code));
}

StatusCode DecodeStatusCode(const value& json) {
  return static_cast<StatusCode>(static_cast<unsigned>(RequireUInt64(json)));
}

// OPC UA Part 6 §5.4.2.5 Guid: the canonical 36-character text form,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.5
value EncodeGuid(const Guid& guid) {
  return string(guid.ToString());
}

Guid DecodeGuid(const value& json) {
  const std::optional<Guid> guid = Guid::FromString(RequireString(json));
  if (!guid.has_value())
    ThrowJsonError("Invalid Guid");
  return *guid;
}

// OPC UA Part 6 §5.4.2.8 XmlElement: the XML text as a JSON string,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.8
value EncodeXmlElement(const XmlElement& element) {
  return string(element.value);
}

XmlElement DecodeXmlElement(const value& json) {
  return XmlElement{std::string{RequireString(json)}};
}

// OPC UA Part 6 §5.4.2.13 DiagnosticInfo: an object whose fields are omitted
// when absent,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.13 The field
// names follow the OPC Foundation's published JSON schema
// (opc.ua.services.jsonschema.json), which spells the namespace field
// `NamespaceUri` — unlike the binary type dictionary's `NamespaceURI`.
value EncodeDiagnosticInfo(const DiagnosticInfo& info) {
  object json;
  if (info.symbolic_id.has_value())
    json["SymbolicId"] = *info.symbolic_id;
  if (info.namespace_uri.has_value())
    json["NamespaceUri"] = *info.namespace_uri;
  if (info.locale.has_value())
    json["Locale"] = *info.locale;
  if (info.localized_text.has_value())
    json["LocalizedText"] = *info.localized_text;
  if (info.additional_info.has_value())
    json["AdditionalInfo"] = *info.additional_info;
  if (info.inner_status_code.has_value())
    json["InnerStatusCode"] = EncodeStatus(*info.inner_status_code);
  if (info.inner_diagnostic_info != nullptr)
    json["InnerDiagnosticInfo"] =
        EncodeDiagnosticInfo(*info.inner_diagnostic_info);
  return json;
}

DiagnosticInfo DecodeDiagnosticInfo(const value& json) {
  const auto& obj = RequireObject(json);
  DiagnosticInfo info;
  if (const auto* field = FindField(obj, "SymbolicId"))
    info.symbolic_id = static_cast<Int32>(RequireInt64(*field));
  if (const auto* field = FindField(obj, "NamespaceUri"))
    info.namespace_uri = static_cast<Int32>(RequireInt64(*field));
  if (const auto* field = FindField(obj, "Locale"))
    info.locale = static_cast<Int32>(RequireInt64(*field));
  if (const auto* field = FindField(obj, "LocalizedText"))
    info.localized_text = static_cast<Int32>(RequireInt64(*field));
  if (const auto* field = FindField(obj, "AdditionalInfo"))
    info.additional_info = std::string{RequireString(*field)};
  if (const auto* field = FindField(obj, "InnerStatusCode"))
    info.inner_status_code = DecodeStatus(*field);
  if (const auto* field = FindField(obj, "InnerDiagnosticInfo")) {
    info.inner_diagnostic_info =
        std::make_shared<const DiagnosticInfo>(DecodeDiagnosticInfo(*field));
  }
  return info;
}

value EncodeVariant(const Variant& variant);
Variant DecodeVariant(const value& json);

template <class T, class Encoder>
array EncodeList(const std::vector<T>& values, Encoder&& encoder);

template <class T, class Decoder>
std::vector<T> DecodeList(const value& json, Decoder&& decoder);

// OPC UA Part 6 §5.4.2.17: DataValue is an object with optional fields
// `Value`, `Status`, `SourceTimestamp`, `SourcePicoseconds`,
// `ServerTimestamp`, `ServerPicoseconds`. Each field is omitted when the
// underlying value is at its default. Notes:
//   - Spec uses `Status` (not `StatusCode`) for the StatusCode value.
//   - Picoseconds are omitted (server keeps no sub-microsecond precision).
//   - The scada-specific `Qualifier` has no spec equivalent and is dropped.
value EncodeDataValue(const DataValue& data_value) {
  object json;
  if (data_value.value.type() != Variant::EMPTY)
    json["Value"] = EncodeVariant(data_value.value);
  if (data_value.status_code != StatusCode::Good)
    json["Status"] = EncodeStatusCode(data_value.status_code);
  if (!data_value.source_timestamp.is_null())
    json["SourceTimestamp"] = EncodeDateTime(data_value.source_timestamp);
  if (!data_value.server_timestamp.is_null())
    json["ServerTimestamp"] = EncodeDateTime(data_value.server_timestamp);
  return json;
}

DataValue DecodeDataValue(const value& json) {
  const auto& obj = RequireObject(json);
  DataValue result;
  if (const auto* field = FindField(obj, "Value"))
    result.value = DecodeVariant(*field);
  if (const auto* field = FindField(obj, "Status"))
    result.status_code = DecodeStatusCode(*field);
  if (const auto* field = FindField(obj, "SourceTimestamp"))
    result.source_timestamp = DecodeDateTime(*field);
  if (const auto* field = FindField(obj, "ServerTimestamp"))
    result.server_timestamp = DecodeDateTime(*field);
  return result;
}

value EncodeEventFilter(const EventFilter& filter) {
  array of_type;
  of_type.reserve(filter.of_type.size());
  for (const auto& node_id : filter.of_type)
    of_type.emplace_back(EncodeNodeId(node_id));

  array child_of;
  child_of.reserve(filter.child_of.size());
  for (const auto& node_id : filter.child_of)
    child_of.emplace_back(EncodeNodeId(node_id));

  return object{{"Types", filter.types},
                {"OfType", std::move(of_type)},
                {"ChildOf", std::move(child_of)}};
}

EventFilter DecodeEventFilter(const value& json) {
  const auto& obj = RequireObject(json);
  EventFilter filter;
  filter.types =
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "Types")));
  for (const auto& entry : RequireArray(RequireField(obj, "OfType")))
    filter.of_type.push_back(DecodeNodeId(entry));
  for (const auto& entry : RequireArray(RequireField(obj, "ChildOf")))
    filter.child_of.push_back(DecodeNodeId(entry));
  return filter;
}

value EncodeAggregateFilter(const AggregateFilter& filter) {
  return object{{"StartTime", EncodeDateTime(filter.start_time)},
                {"Interval", filter.interval.InMicroseconds()},
                {"AggregateType", EncodeNodeId(filter.aggregate_type)}};
}

AggregateFilter DecodeAggregateFilter(const value& json) {
  const auto& obj = RequireObject(json);
  return {.start_time = DecodeDateTime(RequireField(obj, "StartTime")),
          .interval = Duration::FromMicroseconds(
              RequireInt64(RequireField(obj, "Interval"))),
          .aggregate_type = DecodeNodeId(RequireField(obj, "AggregateType"))};
}

value EncodeEvent(const Event& event) {
  return object{
      {"EventTypeId", EncodeNodeId(event.event_type_id)},
      {"EventId", event.event_id},
      {"Time", EncodeDateTime(event.time)},
      {"ReceiveTime", EncodeDateTime(event.receive_time)},
      {"ChangeMask", event.change_mask},
      {"Severity", event.severity},
      {"NodeId", EncodeNodeId(event.source_node_id)},
      {"SourceName", event.source_name},
      {"UserId", EncodeNodeId(event.user_id)},
      {"Value", EncodeVariant(event.value)},
      {"Qualifier", event.qualifier.raw()},
      {"Message", EncodeLocalizedText(event.message)},
      {"Acked", event.acked},
      {"AcknowledgedTime", EncodeDateTime(event.acknowledged_time)},
      {"AcknowledgedUserId", EncodeNodeId(event.acknowledged_user_id)},
  };
}

Event DecodeEvent(const value& json) {
  const auto& obj = RequireObject(json);
  Event event;
  event.event_type_id = DecodeNodeId(RequireField(obj, "EventTypeId"));
  event.event_id = RequireUInt64(RequireField(obj, "EventId"));
  event.time = DecodeDateTime(RequireField(obj, "Time"));
  event.receive_time = DecodeDateTime(RequireField(obj, "ReceiveTime"));
  event.change_mask =
      static_cast<UInt32>(RequireUInt64(RequireField(obj, "ChangeMask")));
  event.severity =
      static_cast<UInt32>(RequireUInt64(RequireField(obj, "Severity")));
  event.source_node_id = DecodeNodeId(RequireField(obj, "NodeId"));
  // Optional: absent in payloads from pre-ADR-0005 peers.
  if (const auto* source_name = obj.if_contains("SourceName")) {
    event.source_name = std::string{RequireString(*source_name)};
  }
  event.user_id = DecodeNodeId(RequireField(obj, "UserId"));
  event.value = DecodeVariant(RequireField(obj, "Value"));
  event.qualifier = Qualifier{
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "Qualifier")))};
  event.message = DecodeLocalizedText(RequireField(obj, "Message"));
  event.acked = RequireBool(RequireField(obj, "Acked"));
  event.acknowledged_time =
      DecodeDateTime(RequireField(obj, "AcknowledgedTime"));
  event.acknowledged_user_id =
      DecodeNodeId(RequireField(obj, "AcknowledgedUserId"));
  return event;
}

template <class T, class Encoder>
array EncodeList(const std::vector<T>& values, Encoder&& encoder) {
  array json;
  json.reserve(values.size());
  for (const auto& value : values)
    json.emplace_back(encoder(value));
  return json;
}

template <class T, class Decoder>
std::vector<T> DecodeList(const value& json, Decoder&& decoder) {
  std::vector<T> values;
  const auto& source = RequireArray(json);
  values.reserve(source.size());
  for (const auto& entry : source)
    values.push_back(decoder(entry));
  return values;
}

template <class T>
value EncodeScalarList(const std::vector<T>& values) {
  array json;
  json.reserve(values.size());
  for (const auto& value : values)
    json.emplace_back(value);
  return json;
}

template <class T>
std::vector<T> DecodeIntList(const value& json) {
  std::vector<T> values;
  const auto& source = RequireArray(json);
  values.reserve(source.size());
  for (const auto& entry : source)
    values.push_back(static_cast<T>(RequireInt64(entry)));
  return values;
}

template <class T>
std::vector<T> DecodeUIntList(const value& json) {
  std::vector<T> values;
  const auto& source = RequireArray(json);
  values.reserve(source.size());
  for (const auto& entry : source)
    values.push_back(static_cast<T>(RequireUInt64(entry)));
  return values;
}

// OPC UA Part 6 §5.4.2.16: reversible Variant encoding is
// `{ "Type": <BuiltInTypeId>, "Body": <encoded value>, "Dimensions"?: [...] }`.
// Type is the numeric BuiltInType id (1..25 per Part 3); Body is the encoded
// scalar OR a JSON array of encoded scalars when the Variant is an array.
// IsArray is implicit (Body type) and not on the wire.
namespace {

// Variant::Type enumerators are the spec BuiltInType ids (see variant.h), so
// the wire id needs no translation table — only a range check on the way in.
unsigned BuiltInTypeId(Variant::Type type) {
  return static_cast<unsigned>(type);
}

Variant::Type FromBuiltInTypeId(unsigned id) {
  return id < static_cast<unsigned>(Variant::COUNT)
             ? static_cast<Variant::Type>(id)
             : Variant::COUNT;
}

}  // namespace

value EncodeVariant(const Variant& variant) {
  if (variant.type() == Variant::EMPTY)
    return nullptr;

  object json{{"Type", BuiltInTypeId(variant.type())}};

  if (variant.is_scalar()) {
    switch (variant.type()) {
      case Variant::EMPTY:
        break;
      case Variant::BOOL:
        json["Body"] = variant.get<bool>();
        break;
      case Variant::INT8:
        json["Body"] = variant.get<Int8>();
        break;
      case Variant::UINT8:
        json["Body"] = variant.get<UInt8>();
        break;
      case Variant::INT16:
        json["Body"] = variant.get<Int16>();
        break;
      case Variant::UINT16:
        json["Body"] = variant.get<UInt16>();
        break;
      case Variant::INT32:
        json["Body"] = variant.get<Int32>();
        break;
      case Variant::UINT32:
        json["Body"] = variant.get<UInt32>();
        break;
      case Variant::INT64:
        json["Body"] = variant.get<Int64>();
        break;
      case Variant::UINT64:
        json["Body"] = variant.get<UInt64>();
        break;
      case Variant::DOUBLE:
        json["Body"] = variant.get<double>();
        break;
      case Variant::BYTE_STRING:
        json["Body"] = EncodeByteString(variant.get<ByteString>());
        break;
      case Variant::STRING:
        json["Body"] = variant.get<String>();
        break;
      case Variant::QUALIFIED_NAME:
        json["Body"] = EncodeQualifiedName(variant.get<QualifiedName>());
        break;
      case Variant::LOCALIZED_TEXT:
        json["Body"] = EncodeLocalizedText(variant.get<LocalizedText>());
        break;
      case Variant::NODE_ID:
        json["Body"] = EncodeNodeId(variant.get<NodeId>());
        break;
      case Variant::EXPANDED_NODE_ID:
        json["Body"] = EncodeExpandedNodeId(variant.get<ExpandedNodeId>());
        break;
      case Variant::EXTENSION_OBJECT:
        json["Body"] = EncodeExtensionObject(variant.get<ExtensionObject>());
        break;
      case Variant::DATE_TIME:
        json["Body"] = EncodeDateTime(variant.get<DateTime>());
        break;
      case Variant::FLOAT:
        json["Body"] = variant.get<Float>();
        break;
      case Variant::GUID:
        json["Body"] = EncodeGuid(variant.get<Guid>());
        break;
      case Variant::XML_ELEMENT:
        json["Body"] = EncodeXmlElement(variant.get<XmlElement>());
        break;
      case Variant::STATUS_CODE:
        json["Body"] = EncodeStatus(variant.get<Status>());
        break;
      case Variant::DIAGNOSTIC_INFO:
        json["Body"] = EncodeDiagnosticInfo(variant.get<DiagnosticInfo>());
        break;
      case Variant::DATA_VALUE: {
        const auto& nested = variant.get<std::shared_ptr<const DataValue>>();
        json["Body"] = EncodeDataValue(nested ? *nested : DataValue{});
        break;
      }
      case Variant::VARIANT: {
        const auto& nested = variant.get<std::shared_ptr<const Variant>>();
        json["Body"] = EncodeVariant(nested ? *nested : Variant{});
        break;
      }
      case Variant::COUNT:
        ThrowJsonError("Unexpected scalar variant type");
    }
  } else {
    switch (variant.type()) {
      case Variant::EMPTY:
        json["Body"] =
            array(variant.get<std::vector<std::monostate>>().size(), nullptr);
        break;
      case Variant::BOOL:
        json["Body"] = EncodeScalarList(variant.get<std::vector<bool>>());
        break;
      case Variant::INT8:
        json["Body"] = EncodeScalarList(variant.get<std::vector<Int8>>());
        break;
      case Variant::UINT8:
        json["Body"] = EncodeScalarList(variant.get<std::vector<UInt8>>());
        break;
      case Variant::INT16:
        json["Body"] = EncodeScalarList(variant.get<std::vector<Int16>>());
        break;
      case Variant::UINT16:
        json["Body"] = EncodeScalarList(variant.get<std::vector<UInt16>>());
        break;
      case Variant::INT32:
        json["Body"] = EncodeScalarList(variant.get<std::vector<Int32>>());
        break;
      case Variant::UINT32:
        json["Body"] = EncodeScalarList(variant.get<std::vector<UInt32>>());
        break;
      case Variant::INT64:
        json["Body"] = EncodeScalarList(variant.get<std::vector<Int64>>());
        break;
      case Variant::UINT64:
        json["Body"] = EncodeScalarList(variant.get<std::vector<UInt64>>());
        break;
      case Variant::DOUBLE:
        json["Body"] = EncodeScalarList(variant.get<std::vector<double>>());
        break;
      case Variant::BYTE_STRING:
        json["Body"] = EncodeList(variant.get<std::vector<ByteString>>(),
                                  EncodeByteString);
        break;
      case Variant::STRING:
        json["Body"] = EncodeScalarList(variant.get<std::vector<String>>());
        break;
      case Variant::QUALIFIED_NAME:
        json["Body"] = EncodeList(variant.get<std::vector<QualifiedName>>(),
                                  EncodeQualifiedName);
        break;
      case Variant::LOCALIZED_TEXT:
        json["Body"] = EncodeList(variant.get<std::vector<LocalizedText>>(),
                                  EncodeLocalizedText);
        break;
      case Variant::NODE_ID:
        json["Body"] =
            EncodeList(variant.get<std::vector<NodeId>>(), EncodeNodeId);
        break;
      case Variant::EXPANDED_NODE_ID:
        json["Body"] = EncodeList(variant.get<std::vector<ExpandedNodeId>>(),
                                  EncodeExpandedNodeId);
        break;
      case Variant::DATE_TIME:
        json["Body"] =
            EncodeList(variant.get<std::vector<DateTime>>(), EncodeDateTime);
        break;
      case Variant::EXTENSION_OBJECT:
        json["Body"] = EncodeList(variant.get<std::vector<ExtensionObject>>(),
                                  EncodeExtensionObject);
        break;
      case Variant::FLOAT:
        json["Body"] = EncodeScalarList(variant.get<std::vector<Float>>());
        break;
      case Variant::GUID:
        json["Body"] = EncodeList(variant.get<std::vector<Guid>>(), EncodeGuid);
        break;
      case Variant::XML_ELEMENT:
        json["Body"] = EncodeList(variant.get<std::vector<XmlElement>>(),
                                  EncodeXmlElement);
        break;
      case Variant::STATUS_CODE:
        json["Body"] =
            EncodeList(variant.get<std::vector<Status>>(), EncodeStatus);
        break;
      case Variant::DIAGNOSTIC_INFO:
        json["Body"] = EncodeList(variant.get<std::vector<DiagnosticInfo>>(),
                                  EncodeDiagnosticInfo);
        break;
      case Variant::DATA_VALUE:
        json["Body"] = EncodeList(
            variant.get<std::vector<std::shared_ptr<const DataValue>>>(),
            [](const std::shared_ptr<const DataValue>& nested) {
              return EncodeDataValue(nested ? *nested : DataValue{});
            });
        break;
      case Variant::VARIANT:
        json["Body"] =
            EncodeList(variant.get<std::vector<Variant>>(), EncodeVariant);
        break;
      case Variant::COUNT:
        ThrowJsonError("Unexpected array variant type");
    }
  }

  return json;
}

Variant DecodeVariant(const value& json) {
  // Spec: a Variant carrying Type 0 (Null) is encoded as the JSON literal
  // null with no Body. Treat top-level null as an empty Variant.
  if (json.is_null())
    return {};

  const auto& obj = RequireObject(json);
  auto type = FromBuiltInTypeId(
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "Type"))));
  if (type == Variant::COUNT)
    ThrowJsonError("Unsupported Variant Type id");

  // No `IsArray` on the wire — derived from the Body's JSON kind.
  const auto* body_field = FindField(obj, "Body");
  if (body_field == nullptr || body_field->is_null())
    return {};
  const auto& payload = *body_field;
  bool is_array = payload.is_array();

  if (!is_array) {
    switch (type) {
      case Variant::EMPTY:
        return {};
      case Variant::BOOL:
        return Variant{RequireBool(payload)};
      case Variant::INT8:
        return Variant{static_cast<Int8>(RequireInt64(payload))};
      case Variant::UINT8:
        return Variant{static_cast<UInt8>(RequireUInt64(payload))};
      case Variant::INT16:
        return Variant{static_cast<Int16>(RequireInt64(payload))};
      case Variant::UINT16:
        return Variant{static_cast<UInt16>(RequireUInt64(payload))};
      case Variant::INT32:
        return Variant{static_cast<Int32>(RequireInt64(payload))};
      case Variant::UINT32:
        return Variant{static_cast<UInt32>(RequireUInt64(payload))};
      case Variant::INT64:
        return Variant{static_cast<Int64>(RequireInt64(payload))};
      case Variant::UINT64:
        return Variant{static_cast<UInt64>(RequireUInt64(payload))};
      case Variant::DOUBLE:
        return Variant{RequireDouble(payload)};
      case Variant::BYTE_STRING:
        return Variant{DecodeByteString(payload)};
      case Variant::STRING:
        return Variant{std::string{RequireString(payload)}};
      case Variant::QUALIFIED_NAME:
        return Variant{DecodeQualifiedName(payload)};
      case Variant::LOCALIZED_TEXT:
        return Variant{DecodeLocalizedText(payload)};
      case Variant::NODE_ID:
        return Variant{DecodeNodeId(payload)};
      case Variant::EXPANDED_NODE_ID:
        return Variant{DecodeExpandedNodeId(payload)};
      case Variant::DATE_TIME:
        return Variant{DecodeDateTime(payload)};
      case Variant::EXTENSION_OBJECT:
        return Variant{DecodeExtensionObject(payload)};
      case Variant::FLOAT:
        return Variant{static_cast<Float>(RequireDouble(payload))};
      case Variant::GUID:
        return Variant{DecodeGuid(payload)};
      case Variant::XML_ELEMENT:
        return Variant{DecodeXmlElement(payload)};
      case Variant::STATUS_CODE:
        return Variant{DecodeStatus(payload)};
      case Variant::DIAGNOSTIC_INFO:
        return Variant{DecodeDiagnosticInfo(payload)};
      case Variant::DATA_VALUE:
        return Variant{
            std::make_shared<const DataValue>(DecodeDataValue(payload))};
      case Variant::VARIANT:
        return Variant{std::make_shared<const Variant>(DecodeVariant(payload))};
      case Variant::COUNT:
        ThrowJsonError("Unsupported scalar variant type");
    }
  }

  switch (type) {
    case Variant::EMPTY:
      return Variant{DecodeList<std::monostate>(
          payload, [](const value&) { return std::monostate{}; })};
    case Variant::BOOL:
      return Variant{DecodeList<bool>(payload, RequireBool)};
    case Variant::INT8:
      return Variant{DecodeIntList<Int8>(payload)};
    case Variant::UINT8:
      return Variant{DecodeUIntList<UInt8>(payload)};
    case Variant::INT16:
      return Variant{DecodeIntList<Int16>(payload)};
    case Variant::UINT16:
      return Variant{DecodeUIntList<UInt16>(payload)};
    case Variant::INT32:
      return Variant{DecodeIntList<Int32>(payload)};
    case Variant::UINT32:
      return Variant{DecodeUIntList<UInt32>(payload)};
    case Variant::INT64:
      return Variant{DecodeIntList<Int64>(payload)};
    case Variant::UINT64:
      return Variant{DecodeUIntList<UInt64>(payload)};
    case Variant::DOUBLE:
      return Variant{DecodeList<double>(payload, RequireDouble)};
    case Variant::BYTE_STRING:
      return Variant{DecodeList<ByteString>(payload, DecodeByteString)};
    case Variant::STRING:
      return Variant{DecodeList<String>(payload, [](const value& v) {
        return std::string{RequireString(v)};
      })};
    case Variant::QUALIFIED_NAME:
      return Variant{DecodeList<QualifiedName>(payload, DecodeQualifiedName)};
    case Variant::LOCALIZED_TEXT:
      return Variant{DecodeList<LocalizedText>(payload, DecodeLocalizedText)};
    case Variant::NODE_ID:
      return Variant{DecodeList<NodeId>(payload, DecodeNodeId)};
    case Variant::EXPANDED_NODE_ID:
      return Variant{DecodeList<ExpandedNodeId>(payload, DecodeExpandedNodeId)};
    case Variant::DATE_TIME:
      return Variant{DecodeList<DateTime>(payload, DecodeDateTime)};
    case Variant::EXTENSION_OBJECT:
      return Variant{
          DecodeList<ExtensionObject>(payload, DecodeExtensionObject)};
    case Variant::FLOAT:
      return Variant{DecodeList<Float>(payload, [](const value& v) {
        return static_cast<Float>(RequireDouble(v));
      })};
    case Variant::GUID:
      return Variant{DecodeList<Guid>(payload, DecodeGuid)};
    case Variant::XML_ELEMENT:
      return Variant{DecodeList<XmlElement>(payload, DecodeXmlElement)};
    case Variant::STATUS_CODE:
      return Variant{DecodeList<Status>(payload, DecodeStatus)};
    case Variant::DIAGNOSTIC_INFO:
      return Variant{DecodeList<DiagnosticInfo>(payload, DecodeDiagnosticInfo)};
    case Variant::DATA_VALUE:
      return Variant{DecodeList<std::shared_ptr<const DataValue>>(
          payload, [](const value& v) {
            return std::make_shared<const DataValue>(DecodeDataValue(v));
          })};
    case Variant::VARIANT:
      return Variant{DecodeList<Variant>(payload, DecodeVariant)};
    case Variant::COUNT:
      ThrowJsonError("Unsupported array variant type");
  }

  ThrowJsonError("Unsupported variant type");
}

// The websocket keeps distinct HistoryReadRaw / HistoryReadEvents /
// HistoryUpdate service names and their web-shaped bodies (unchanged for
// compatibility); the codec translates to/from the generated ua:: wire messages
// via history_conversion, so the internal variant stays uniformly ua::.

value EncodeHistoryReadRawRequestBody(const HistoryReadRawDetails& details) {
  return object{
      {"Details",
       object{{"NodeId", EncodeNodeId(details.node_id)},
              {"From", EncodeDateTime(details.from)},
              {"To", EncodeDateTime(details.to)},
              {"MaxCount", details.max_count},
              {"Aggregation", EncodeAggregateFilter(details.aggregation)},
              {"ReleaseContinuationPoint", details.release_continuation_point},
              {"ContinuationPoint",
               EncodeByteString(details.continuation_point)}}}};
}

ua::HistoryReadRequest DecodeHistoryReadRawRequest(const value& json) {
  const auto& details =
      RequireObject(RequireField(RequireObject(json), "Details"));
  return history_conversion::ToWireRawRequest(
      {.node_id = DecodeNodeId(RequireField(details, "NodeId")),
       .from = DecodeDateTime(RequireField(details, "From")),
       .to = DecodeDateTime(RequireField(details, "To")),
       .max_count = static_cast<size_t>(
           RequireUInt64(RequireField(details, "MaxCount"))),
       .aggregation =
           DecodeAggregateFilter(RequireField(details, "Aggregation")),
       .release_continuation_point =
           RequireBool(RequireField(details, "ReleaseContinuationPoint")),
       .continuation_point =
           DecodeByteString(RequireField(details, "ContinuationPoint"))});
}

value EncodeHistoryReadEventsRequestBody(
    const HistoryReadEventsDetails& details) {
  return object{
      {"Details", object{{"NodeId", EncodeNodeId(details.node_id)},
                         {"From", EncodeDateTime(details.from)},
                         {"To", EncodeDateTime(details.to)},
                         {"Filter", EncodeEventFilter(details.filter)}}}};
}

ua::HistoryReadRequest DecodeHistoryReadEventsRequest(const value& json) {
  const auto& details =
      RequireObject(RequireField(RequireObject(json), "Details"));
  return history_conversion::ToWireEventsRequest(
      {.node_id = DecodeNodeId(RequireField(details, "NodeId")),
       .from = DecodeDateTime(RequireField(details, "From")),
       .to = DecodeDateTime(RequireField(details, "To")),
       .filter = DecodeEventFilter(RequireField(details, "Filter"))});
}

value EncodeHistoryReadRawResponseBody(
    const StatusOr<HistoryReadRawResult>& result) {
  object encoded{{"Status", EncodeStatus(result.status())}};
  // A failed read carries no data; keep the fields present (empty) so the
  // decoder's shape stays unconditional.
  encoded["Values"] =
      result.ok() ? EncodeList(result->values, EncodeDataValue) : array{};
  encoded["ContinuationPoint"] =
      EncodeByteString(result.ok() ? result->continuation_point : ByteString{});
  return object{{"Result", std::move(encoded)}};
}

ua::HistoryReadResponse DecodeHistoryReadRawResponse(const value& json) {
  const auto& result =
      RequireObject(RequireField(RequireObject(json), "Result"));
  const Status status = DecodeStatus(RequireField(result, "Status"));
  if (!status) {
    return history_conversion::ToWireRawResponse(status);
  }
  return history_conversion::ToWireRawResponse(HistoryReadRawResult{
      .values = DecodeList<DataValue>(RequireField(result, "Values"),
                                      DecodeDataValue),
      .continuation_point =
          DecodeByteString(RequireField(result, "ContinuationPoint"))});
}

value EncodeHistoryReadEventsResponseBody(
    const StatusOr<HistoryReadEventsResult>& result) {
  // A failed read carries no events; keep the field present (empty) so the
  // decoder's shape stays unconditional.
  return object{
      {"Result",
       object{{"Status", EncodeStatus(result.status())},
              {"Events", result.ok() ? EncodeList(result->events, EncodeEvent)
                                     : array{}}}}};
}

ua::HistoryReadResponse DecodeHistoryReadEventsResponse(const value& json) {
  const auto& result =
      RequireObject(RequireField(RequireObject(json), "Result"));
  const Status status = DecodeStatus(RequireField(result, "Status"));
  if (!status) {
    return history_conversion::ToWireEventsResponse(status,
                                                    DefaultEventFieldPaths());
  }
  return history_conversion::ToWireEventsResponse(
      HistoryReadEventsResult{.events = DecodeList<Event>(
                                  RequireField(result, "Events"), DecodeEvent)},
      DefaultEventFieldPaths());
}

value EncodeHistoryUpdateRequestBody(
    const history_conversion::HistoryUpdateDetails& details) {
  // The detail is data or event; an "Events" field (vs "Values") discriminates
  // the kind on decode.
  if (const auto* data = std::get_if<UpdateDataDetails>(&details)) {
    return object{
        {"Details",
         object{{"NodeId", EncodeNodeId(data->node_id)},
                {"PerformInsertReplace",
                 static_cast<int>(data->perform_insert_replace)},
                {"Values", EncodeList(data->values, EncodeDataValue)}}}};
  }
  const auto& event_details = std::get<UpdateEventDetails>(details);
  return object{
      {"Details",
       object{{"NodeId", EncodeNodeId(event_details.node_id)},
              {"PerformInsertReplace",
               static_cast<int>(event_details.perform_insert_replace)},
              {"Events", EncodeList(event_details.events, EncodeEvent)}}}};
}

ua::HistoryUpdateRequest DecodeHistoryUpdateRequest(const value& json) {
  const auto& details =
      RequireObject(RequireField(RequireObject(json), "Details"));
  history_conversion::HistoryUpdateDetails managed;
  if (const auto* events = details.if_contains("Events")) {
    managed = UpdateEventDetails{
        .node_id = DecodeNodeId(RequireField(details, "NodeId")),
        .perform_insert_replace = static_cast<PerformUpdateType>(
            RequireUInt64(RequireField(details, "PerformInsertReplace"))),
        .events = DecodeList<Event>(*events, DecodeEvent)};
  } else {
    managed = UpdateDataDetails{
        .node_id = DecodeNodeId(RequireField(details, "NodeId")),
        .perform_insert_replace = static_cast<PerformUpdateType>(
            RequireUInt64(RequireField(details, "PerformInsertReplace"))),
        .values = DecodeList<DataValue>(RequireField(details, "Values"),
                                        DecodeDataValue)};
  }
  return history_conversion::ToWire(managed);
}

value EncodeHistoryUpdateResponseBody(const HistoryUpdateResult& result) {
  return object{{"Result", object{{"Status", EncodeStatus(result.status)},
                                  {"OperationResults",
                                   EncodeList(result.operation_results,
                                              EncodeStatusCode)}}}};
}

ua::HistoryUpdateResponse DecodeHistoryUpdateResponse(const value& json) {
  const auto& result =
      RequireObject(RequireField(RequireObject(json), "Result"));
  return history_conversion::ToWire(HistoryUpdateResult{
      .status = DecodeStatus(RequireField(result, "Status")),
      .operation_results = DecodeList<StatusCode>(
          RequireField(result, "OperationResults"), DecodeStatusCode)});
}

template <class Response>
value EncodeMultiStatusResponse(const Response& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"Results", EncodeList(response.results, EncodeStatusCode)}};
}

template <class Response>
Response DecodeMultiStatusResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<StatusCode>(RequireField(obj, "Results"),
                                            DecodeStatusCode)};
}

}  // namespace

boost::json::value EncodeJson(const ServiceRequest& request) {
  return std::visit(
      [](const auto& typed_request) -> value {
        using T = std::decay_t<decltype(typed_request)>;
        object json;
        // HistoryRead/HistoryUpdate keep distinct web service names and body
        // shapes (Path Y); everything else travels under the type's own
        // kServiceName with the conformant ua:: JSON body.
        if constexpr (std::is_same_v<T, ua::HistoryReadRequest>) {
          auto decoded = history_conversion::ToManaged(typed_request);
          if (decoded && std::holds_alternative<HistoryReadEventsDetails>(
                             decoded->details)) {
            json["service"] = "HistoryReadEvents";
            json["body"] = EncodeHistoryReadEventsRequestBody(
                std::get<HistoryReadEventsDetails>(decoded->details));
          } else {
            json["service"] = "HistoryReadRaw";
            json["body"] = EncodeHistoryReadRawRequestBody(
                decoded ? std::get<HistoryReadRawDetails>(decoded->details)
                        : HistoryReadRawDetails{});
          }
        } else if constexpr (std::is_same_v<T, ua::HistoryUpdateRequest>) {
          auto detail = history_conversion::ToManaged(typed_request);
          json["service"] = "HistoryUpdate";
          json["body"] = EncodeHistoryUpdateRequestBody(
              detail ? *detail
                     : history_conversion::HistoryUpdateDetails{
                           UpdateDataDetails{}});
        } else {
          json["service"] = T::kServiceName;
          json["body"] = ua::EncodeJson(typed_request);
        }
        return json;
      },
      request);
}

boost::json::value EncodeJson(const ServiceResponse& response) {
  return std::visit(
      [](const auto& typed_response) -> value {
        using T = std::decay_t<decltype(typed_response)>;
        object json;
        if constexpr (std::is_same_v<T, ua::HistoryReadResponse>) {
          // Emit the web service name + body matching the payload kind.
          if (history_conversion::IsEventsResponse(typed_response)) {
            json["service"] = "HistoryReadEvents";
            json["body"] = EncodeHistoryReadEventsResponseBody(
                history_conversion::ToManagedEventsResult(typed_response));
          } else {
            json["service"] = "HistoryReadRaw";
            json["body"] = EncodeHistoryReadRawResponseBody(
                history_conversion::ToManagedRawResult(typed_response));
          }
        } else if constexpr (std::is_same_v<T, ua::HistoryUpdateResponse>) {
          json["service"] = "HistoryUpdate";
          json["body"] = EncodeHistoryUpdateResponseBody(
              history_conversion::ToManaged(typed_response));
        } else {
          json["service"] = T::kServiceName;
          json["body"] = ua::EncodeJson(typed_response);
        }
        return json;
      },
      response);
}

namespace {

// Decodes a conformant ua:: JSON body when the envelope's service matches the
// type's own kServiceName; nullopt otherwise. Folded over the pure-ua services
// so the dispatch has no per-service arm or hand-written service string.
template <class T>
std::optional<ServiceRequest> TryJsonRequest(std::string_view service,
                                             const boost::json::value& body) {
  if (service != T::kServiceName)
    return std::nullopt;
  T request;
  ua::DecodeJson(body, request);
  return ServiceRequest{std::move(request)};
}

template <class... Ts>
std::optional<ServiceRequest> DecodeAnyJsonRequest(
    std::string_view service,
    const boost::json::value& body) {
  std::optional<ServiceRequest> result;
  (void)((result = TryJsonRequest<Ts>(service, body)).has_value() || ...);
  return result;
}

template <class T>
std::optional<ServiceResponse> TryJsonResponse(std::string_view service,
                                               const boost::json::value& body) {
  if (service != T::kServiceName)
    return std::nullopt;
  T response;
  ua::DecodeJson(body, response);
  return ServiceResponse{std::move(response)};
}

template <class... Ts>
std::optional<ServiceResponse> DecodeAnyJsonResponse(
    std::string_view service,
    const boost::json::value& body) {
  std::optional<ServiceResponse> result;
  (void)((result = TryJsonResponse<Ts>(service, body)).has_value() || ...);
  return result;
}

}  // namespace

StatusOr<ServiceRequest> DecodeServiceRequest(const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    auto service = RequireString(RequireField(obj, "service"));
    // HistoryRead/HistoryUpdate travel under distinct web names (Path Y).
    if (service == "HistoryReadRaw")
      return ServiceRequest{DecodeHistoryReadRawRequest(body)};
    if (service == "HistoryReadEvents")
      return ServiceRequest{DecodeHistoryReadEventsRequest(body)};
    if (service == "HistoryUpdate")
      return ServiceRequest{DecodeHistoryUpdateRequest(body)};
    if (auto request = DecodeAnyJsonRequest<
            ua::ReadRequest, ua::WriteRequest, ua::BrowseRequest,
            ua::BrowseNextRequest, ua::TranslateBrowsePathsToNodeIdsRequest,
            ua::CallRequest, ua::AddNodesRequest, ua::DeleteNodesRequest,
            ua::AddReferencesRequest, ua::DeleteReferencesRequest>(service,
                                                                   body))
      return *request;
    return Status{StatusCode::Bad_TypeMismatch};
  } catch (...) {
    return Status{StatusCode::Bad_TypeMismatch};
  }
}

StatusOr<ServiceResponse> DecodeServiceResponse(
    const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    auto service = RequireString(RequireField(obj, "service"));
    if (service == "HistoryReadRaw")
      return ServiceResponse{DecodeHistoryReadRawResponse(body)};
    if (service == "HistoryReadEvents")
      return ServiceResponse{DecodeHistoryReadEventsResponse(body)};
    if (service == "HistoryUpdate")
      return ServiceResponse{DecodeHistoryUpdateResponse(body)};
    if (auto response = DecodeAnyJsonResponse<
            ua::ReadResponse, ua::WriteResponse, ua::BrowseResponse,
            ua::BrowseNextResponse, ua::TranslateBrowsePathsToNodeIdsResponse,
            ua::CallResponse, ua::AddNodesResponse, ua::DeleteNodesResponse,
            ua::AddReferencesResponse, ua::DeleteReferencesResponse>(service,
                                                                     body))
      return *response;
    return Status{StatusCode::Bad_TypeMismatch};
  } catch (...) {
    return Status{StatusCode::Bad_TypeMismatch};
  }
}

}  // namespace opcua::ws
