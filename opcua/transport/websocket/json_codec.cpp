#include "opcua/transport/websocket/json_codec.h"

#include "opcua/base/time_utils.h"
#include "opcua/base/utf_convert.h"
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

value EncodeNodeClass(NodeClass node_class) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(node_class));
}

NodeClass DecodeNodeClass(const value& json) {
  return static_cast<NodeClass>(static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeAttributeId(AttributeId attribute_id) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(attribute_id));
}

AttributeId DecodeAttributeId(const value& json) {
  return static_cast<AttributeId>(static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeRelativePathElement(const RelativePathElement& path_element) {
  return object{
      {"ReferenceTypeId", EncodeNodeId(path_element.reference_type_id)},
      {"Inverse", path_element.inverse},
      {"IncludeSubtypes", path_element.include_subtypes},
      {"TargetName", EncodeQualifiedName(path_element.target_name)}};
}

RelativePathElement DecodeRelativePathElement(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .reference_type_id = DecodeNodeId(RequireField(obj, "ReferenceTypeId")),
      .inverse = RequireBool(RequireField(obj, "Inverse")),
      .include_subtypes = RequireBool(RequireField(obj, "IncludeSubtypes")),
      .target_name = DecodeQualifiedName(RequireField(obj, "TargetName"))};
}

value EncodeBrowsePath(const BrowsePath& path) {
  return object{{"NodeId", EncodeNodeId(path.node_id)},
                {"RelativePath",
                 EncodeList(path.relative_path, EncodeRelativePathElement)}};
}

BrowsePath DecodeBrowsePath(const value& json) {
  const auto& obj = RequireObject(json);
  return {.node_id = DecodeNodeId(RequireField(obj, "NodeId")),
          .relative_path = DecodeList<RelativePathElement>(
              RequireField(obj, "RelativePath"), DecodeRelativePathElement)};
}

value EncodeBrowsePathTarget(const BrowsePathTarget& target) {
  return object{{"TargetId", EncodeExpandedNodeId(target.target_id)},
                {"RemainingPathIndex", target.remaining_path_index}};
}

BrowsePathTarget DecodeBrowsePathTarget(const value& json) {
  const auto& obj = RequireObject(json);
  return {.target_id = DecodeExpandedNodeId(RequireField(obj, "TargetId")),
          .remaining_path_index = static_cast<size_t>(
              RequireUInt64(RequireField(obj, "RemainingPathIndex")))};
}

value EncodeBrowsePathResult(const BrowsePathResult& result) {
  return object{
      {"StatusCode", EncodeStatusCode(result.status_code)},
      {"Targets", EncodeList(result.targets, EncodeBrowsePathTarget)}};
}

BrowsePathResult DecodeBrowsePathResult(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status_code = DecodeStatusCode(RequireField(obj, "StatusCode")),
          .targets = DecodeList<BrowsePathTarget>(RequireField(obj, "Targets"),
                                                  DecodeBrowsePathTarget)};
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

value EncodeReadRequest(const ua::ReadRequest& request) {
  return ua::EncodeJson(request);
}

ua::ReadRequest DecodeReadRequest(const value& json) {
  ua::ReadRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeWriteRequest(const ua::WriteRequest& request) {
  return ua::EncodeJson(request);
}

ua::WriteRequest DecodeWriteRequest(const value& json) {
  ua::WriteRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeBrowseRequest(const ua::BrowseRequest& request) {
  return ua::EncodeJson(request);
}

ua::BrowseRequest DecodeBrowseRequest(const value& json) {
  ua::BrowseRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeBrowseNextRequest(const ua::BrowseNextRequest& request) {
  return ua::EncodeJson(request);
}

ua::BrowseNextRequest DecodeBrowseNextRequest(const value& json) {
  ua::BrowseNextRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeTranslateBrowsePathsRequest(
    const TranslateBrowsePathsRequest& request) {
  return object{{"BrowsePaths", EncodeList(request.inputs, EncodeBrowsePath)}};
}

TranslateBrowsePathsRequest DecodeTranslateBrowsePathsRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "BrowsePaths");
  if (!field)
    field = FindField(obj, "Inputs");
  if (!field)
    ThrowJsonError("Missing BrowsePaths");
  return {.inputs = DecodeList<BrowsePath>(*field, DecodeBrowsePath)};
}

value EncodeHistoryReadRawRequest(const HistoryReadRawRequest& request) {
  return object{
      {"Details",
       object{
           {"NodeId", EncodeNodeId(request.details.node_id)},
           {"From", EncodeDateTime(request.details.from)},
           {"To", EncodeDateTime(request.details.to)},
           {"MaxCount", request.details.max_count},
           {"Aggregation", EncodeAggregateFilter(request.details.aggregation)},
           {"ReleaseContinuationPoint",
            request.details.release_continuation_point},
           {"ContinuationPoint",
            EncodeByteString(request.details.continuation_point)}}}};
}

HistoryReadRawRequest DecodeHistoryReadRawRequest(const value& json) {
  const auto& details =
      RequireObject(RequireField(RequireObject(json), "Details"));
  return {.details = {.node_id = DecodeNodeId(RequireField(details, "NodeId")),
                      .from = DecodeDateTime(RequireField(details, "From")),
                      .to = DecodeDateTime(RequireField(details, "To")),
                      .max_count = static_cast<size_t>(
                          RequireUInt64(RequireField(details, "MaxCount"))),
                      .aggregation = DecodeAggregateFilter(
                          RequireField(details, "Aggregation")),
                      .release_continuation_point = RequireBool(
                          RequireField(details, "ReleaseContinuationPoint")),
                      .continuation_point = DecodeByteString(
                          RequireField(details, "ContinuationPoint"))}};
}

value EncodeHistoryReadEventsRequest(const HistoryReadEventsRequest& request) {
  return object{
      {"Details",
       object{{"NodeId", EncodeNodeId(request.details.node_id)},
              {"From", EncodeDateTime(request.details.from)},
              {"To", EncodeDateTime(request.details.to)},
              {"Filter", EncodeEventFilter(request.details.filter)}}}};
}

HistoryReadEventsRequest DecodeHistoryReadEventsRequest(const value& json) {
  const auto& details =
      RequireObject(RequireField(RequireObject(json), "Details"));
  return {.details = {
              .node_id = DecodeNodeId(RequireField(details, "NodeId")),
              .from = DecodeDateTime(RequireField(details, "From")),
              .to = DecodeDateTime(RequireField(details, "To")),
              .filter = DecodeEventFilter(RequireField(details, "Filter"))}};
}

value EncodeCallRequest(const ua::CallRequest& request) {
  return ua::EncodeJson(request);
}

ua::CallRequest DecodeCallRequest(const value& json) {
  ua::CallRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeAddNodesRequest(const ua::AddNodesRequest& request) {
  return ua::EncodeJson(request);
}

ua::AddNodesRequest DecodeAddNodesRequest(const value& json) {
  ua::AddNodesRequest request;
  ua::DecodeJson(json, request);
  return request;
}

// Migrated to the generated conformant OPC UA JSON codec (Part 6 §5.4).
value EncodeDeleteNodesRequest(const ua::DeleteNodesRequest& request) {
  return ua::EncodeJson(request);
}

ua::DeleteNodesRequest DecodeDeleteNodesRequest(const value& json) {
  ua::DeleteNodesRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeAddReferencesRequest(const ua::AddReferencesRequest& request) {
  return ua::EncodeJson(request);
}

ua::AddReferencesRequest DecodeAddReferencesRequest(const value& json) {
  ua::AddReferencesRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeDeleteReferencesRequest(
    const ua::DeleteReferencesRequest& request) {
  return ua::EncodeJson(request);
}

ua::DeleteReferencesRequest DecodeDeleteReferencesRequest(const value& json) {
  ua::DeleteReferencesRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeBrowseResponse(const ua::BrowseResponse& response) {
  return ua::EncodeJson(response);
}

ua::BrowseResponse DecodeBrowseResponse(const value& json) {
  ua::BrowseResponse response;
  ua::DecodeJson(json, response);
  return response;
}

value EncodeBrowseNextResponse(const ua::BrowseNextResponse& response) {
  return ua::EncodeJson(response);
}

ua::BrowseNextResponse DecodeBrowseNextResponse(const value& json) {
  ua::BrowseNextResponse response;
  ua::DecodeJson(json, response);
  return response;
}

value EncodeTranslateBrowsePathsResponse(
    const TranslateBrowsePathsResponse& response) {
  return object{
      {"Status", EncodeStatus(response.status)},
      {"Results", EncodeList(response.results, EncodeBrowsePathResult)}};
}

TranslateBrowsePathsResponse DecodeTranslateBrowsePathsResponse(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<BrowsePathResult>(RequireField(obj, "Results"),
                                                  DecodeBrowsePathResult)};
}

value EncodeHistoryReadRawResponse(const HistoryReadRawResponse& response) {
  return object{
      {"Result",
       object{{"Status", EncodeStatus(response.result.status)},
              {"Values", EncodeList(response.result.values, EncodeDataValue)},
              {"ContinuationPoint",
               EncodeByteString(response.result.continuation_point)}}}};
}

HistoryReadRawResponse DecodeHistoryReadRawResponse(const value& json) {
  const auto& result =
      RequireObject(RequireField(RequireObject(json), "Result"));
  return {.result = {.status = DecodeStatus(RequireField(result, "Status")),
                     .values = DecodeList<DataValue>(
                         RequireField(result, "Values"), DecodeDataValue),
                     .continuation_point = DecodeByteString(
                         RequireField(result, "ContinuationPoint"))}};
}

value EncodeHistoryReadEventsResponse(
    const HistoryReadEventsResponse& response) {
  return object{
      {"Result",
       object{{"Status", EncodeStatus(response.result.status)},
              {"Events", EncodeList(response.result.events, EncodeEvent)}}}};
}

HistoryReadEventsResponse DecodeHistoryReadEventsResponse(const value& json) {
  const auto& result =
      RequireObject(RequireField(RequireObject(json), "Result"));
  return {.result = {.status = DecodeStatus(RequireField(result, "Status")),
                     .events = DecodeList<Event>(RequireField(result, "Events"),
                                                 DecodeEvent)}};
}

value EncodeHistoryUpdateRequest(const HistoryUpdateRequest& request) {
  // The detail is data or event; an "Events" field (vs "Values") discriminates
  // the kind on decode.
  if (const auto* data = std::get_if<UpdateDataDetails>(&request.details)) {
    return object{
        {"Details",
         object{{"NodeId", EncodeNodeId(data->node_id)},
                {"PerformInsertReplace",
                 static_cast<int>(data->perform_insert_replace)},
                {"Values", EncodeList(data->values, EncodeDataValue)}}}};
  }
  const auto& event_details = std::get<UpdateEventDetails>(request.details);
  return object{
      {"Details",
       object{{"NodeId", EncodeNodeId(event_details.node_id)},
              {"PerformInsertReplace",
               static_cast<int>(event_details.perform_insert_replace)},
              {"Events", EncodeList(event_details.events, EncodeEvent)}}}};
}

HistoryUpdateRequest DecodeHistoryUpdateRequest(const value& json) {
  const auto& details =
      RequireObject(RequireField(RequireObject(json), "Details"));
  if (const auto* events = details.if_contains("Events")) {
    return {
        .details = UpdateEventDetails{
            .node_id = DecodeNodeId(RequireField(details, "NodeId")),
            .perform_insert_replace = static_cast<PerformUpdateType>(
                RequireUInt64(RequireField(details, "PerformInsertReplace"))),
            .events = DecodeList<Event>(*events, DecodeEvent)}};
  }
  return {.details = UpdateDataDetails{
              .node_id = DecodeNodeId(RequireField(details, "NodeId")),
              .perform_insert_replace = static_cast<PerformUpdateType>(
                  RequireUInt64(RequireField(details, "PerformInsertReplace"))),
              .values = DecodeList<DataValue>(RequireField(details, "Values"),
                                              DecodeDataValue)}};
}

value EncodeHistoryUpdateResponse(const HistoryUpdateResponse& response) {
  return object{
      {"Result",
       object{{"Status", EncodeStatus(response.result.status)},
              {"OperationResults", EncodeList(response.result.operation_results,
                                              EncodeStatusCode)}}}};
}

HistoryUpdateResponse DecodeHistoryUpdateResponse(const value& json) {
  const auto& result =
      RequireObject(RequireField(RequireObject(json), "Result"));
  return {.result = {
              .status = DecodeStatus(RequireField(result, "Status")),
              .operation_results = DecodeList<StatusCode>(
                  RequireField(result, "OperationResults"), DecodeStatusCode)}};
}

value EncodeCallResponse(const ua::CallResponse& response) {
  return ua::EncodeJson(response);
}

ua::CallResponse DecodeCallResponse(const value& json) {
  ua::CallResponse response;
  ua::DecodeJson(json, response);
  return response;
}

value EncodeAddNodesResponse(const ua::AddNodesResponse& response) {
  return ua::EncodeJson(response);
}

ua::AddNodesResponse DecodeAddNodesResponse(const value& json) {
  ua::AddNodesResponse response;
  ua::DecodeJson(json, response);
  return response;
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

template <class T>
constexpr std::string_view RequestServiceName();

template <>
constexpr std::string_view RequestServiceName<ua::ReadRequest>() {
  return "Read";
}
template <>
constexpr std::string_view RequestServiceName<ua::WriteRequest>() {
  return "Write";
}
template <>
constexpr std::string_view RequestServiceName<ua::BrowseRequest>() {
  return "Browse";
}
template <>
constexpr std::string_view RequestServiceName<ua::BrowseNextRequest>() {
  return "BrowseNext";
}
template <>
constexpr std::string_view RequestServiceName<TranslateBrowsePathsRequest>() {
  return "TranslateBrowsePathsToNodeIds";
}
template <>
constexpr std::string_view RequestServiceName<ua::CallRequest>() {
  return "Call";
}
template <>
constexpr std::string_view RequestServiceName<HistoryReadRawRequest>() {
  return "HistoryReadRaw";
}
template <>
constexpr std::string_view RequestServiceName<HistoryReadEventsRequest>() {
  return "HistoryReadEvents";
}
template <>
constexpr std::string_view RequestServiceName<HistoryUpdateRequest>() {
  return "HistoryUpdate";
}
template <>
constexpr std::string_view RequestServiceName<ua::AddNodesRequest>() {
  return "AddNodes";
}
template <>
constexpr std::string_view RequestServiceName<ua::DeleteNodesRequest>() {
  return "DeleteNodes";
}
template <>
constexpr std::string_view RequestServiceName<ua::AddReferencesRequest>() {
  return "AddReferences";
}
template <>
constexpr std::string_view RequestServiceName<ua::DeleteReferencesRequest>() {
  return "DeleteReferences";
}

}  // namespace

boost::json::value EncodeJson(const ServiceRequest& request) {
  return std::visit(
      [](const auto& typed_request) -> value {
        object json;
        json["service"] =
            RequestServiceName<std::decay_t<decltype(typed_request)>>();
        if constexpr (std::is_same_v<std::decay_t<decltype(typed_request)>,
                                     ua::ReadRequest>) {
          json["body"] = EncodeReadRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::WriteRequest>) {
          json["body"] = EncodeWriteRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::BrowseRequest>) {
          json["body"] = EncodeBrowseRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::BrowseNextRequest>) {
          json["body"] = EncodeBrowseNextRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 TranslateBrowsePathsRequest>) {
          json["body"] = EncodeTranslateBrowsePathsRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::CallRequest>) {
          json["body"] = EncodeCallRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 HistoryReadRawRequest>) {
          json["body"] = EncodeHistoryReadRawRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 HistoryReadEventsRequest>) {
          json["body"] = EncodeHistoryReadEventsRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 HistoryUpdateRequest>) {
          json["body"] = EncodeHistoryUpdateRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::AddNodesRequest>) {
          json["body"] = EncodeAddNodesRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::DeleteNodesRequest>) {
          json["body"] = EncodeDeleteNodesRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 ua::AddReferencesRequest>) {
          json["body"] = EncodeAddReferencesRequest(typed_request);
        } else {
          json["body"] = EncodeDeleteReferencesRequest(typed_request);
        }
        return json;
      },
      request);
}

boost::json::value EncodeJson(const ServiceResponse& response) {
  return std::visit(
      [](const auto& typed_response) -> value {
        object json;
        using T = std::decay_t<decltype(typed_response)>;
        if constexpr (std::is_same_v<T, ua::ReadResponse>) {
          json["service"] = "Read";
          json["body"] = ua::EncodeJson(typed_response);
        } else if constexpr (std::is_same_v<T, ua::WriteResponse>) {
          json["service"] = "Write";
          json["body"] = ua::EncodeJson(typed_response);
        } else if constexpr (std::is_same_v<T, ua::BrowseResponse>) {
          json["service"] = "Browse";
          json["body"] = EncodeBrowseResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ua::BrowseNextResponse>) {
          json["service"] = "BrowseNext";
          json["body"] = EncodeBrowseNextResponse(typed_response);
        } else if constexpr (std::is_same_v<T, TranslateBrowsePathsResponse>) {
          json["service"] = "TranslateBrowsePathsToNodeIds";
          json["body"] = EncodeTranslateBrowsePathsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ua::CallResponse>) {
          json["service"] = "Call";
          json["body"] = EncodeCallResponse(typed_response);
        } else if constexpr (std::is_same_v<T, HistoryReadRawResponse>) {
          json["service"] = "HistoryReadRaw";
          json["body"] = EncodeHistoryReadRawResponse(typed_response);
        } else if constexpr (std::is_same_v<T, HistoryReadEventsResponse>) {
          json["service"] = "HistoryReadEvents";
          json["body"] = EncodeHistoryReadEventsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, HistoryUpdateResponse>) {
          json["service"] = "HistoryUpdate";
          json["body"] = EncodeHistoryUpdateResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ua::AddNodesResponse>) {
          json["service"] = "AddNodes";
          json["body"] = EncodeAddNodesResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ua::DeleteNodesResponse>) {
          json["service"] = "DeleteNodes";
          json["body"] = ua::EncodeJson(typed_response);
        } else if constexpr (std::is_same_v<T, ua::AddReferencesResponse>) {
          json["service"] = "AddReferences";
          json["body"] = ua::EncodeJson(typed_response);
        } else {
          json["service"] = "DeleteReferences";
          json["body"] = ua::EncodeJson(typed_response);
        }
        return json;
      },
      response);
}

StatusOr<ServiceRequest> DecodeServiceRequest(const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    auto service = RequireString(RequireField(obj, "service"));
    if (service == "Read")
      return ServiceRequest{DecodeReadRequest(body)};
    if (service == "Write")
      return ServiceRequest{DecodeWriteRequest(body)};
    if (service == "Browse")
      return ServiceRequest{DecodeBrowseRequest(body)};
    if (service == "BrowseNext")
      return ServiceRequest{DecodeBrowseNextRequest(body)};
    if (service == "TranslateBrowsePathsToNodeIds")
      return ServiceRequest{DecodeTranslateBrowsePathsRequest(body)};
    if (service == "Call")
      return ServiceRequest{DecodeCallRequest(body)};
    if (service == "HistoryReadRaw")
      return ServiceRequest{DecodeHistoryReadRawRequest(body)};
    if (service == "HistoryReadEvents")
      return ServiceRequest{DecodeHistoryReadEventsRequest(body)};
    if (service == "HistoryUpdate")
      return ServiceRequest{DecodeHistoryUpdateRequest(body)};
    if (service == "AddNodes")
      return ServiceRequest{DecodeAddNodesRequest(body)};
    if (service == "DeleteNodes")
      return ServiceRequest{DecodeDeleteNodesRequest(body)};
    if (service == "AddReferences")
      return ServiceRequest{DecodeAddReferencesRequest(body)};
    if (service == "DeleteReferences")
      return ServiceRequest{DecodeDeleteReferencesRequest(body)};
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
    if (service == "Read") {
      ua::ReadResponse response;
      ua::DecodeJson(body, response);
      return ServiceResponse{std::move(response)};
    }
    if (service == "Write") {
      ua::WriteResponse response;
      ua::DecodeJson(body, response);
      return ServiceResponse{std::move(response)};
    }
    if (service == "Browse")
      return ServiceResponse{DecodeBrowseResponse(body)};
    if (service == "BrowseNext")
      return ServiceResponse{DecodeBrowseNextResponse(body)};
    if (service == "TranslateBrowsePathsToNodeIds")
      return ServiceResponse{DecodeTranslateBrowsePathsResponse(body)};
    if (service == "Call")
      return ServiceResponse{DecodeCallResponse(body)};
    if (service == "HistoryReadRaw")
      return ServiceResponse{DecodeHistoryReadRawResponse(body)};
    if (service == "HistoryReadEvents")
      return ServiceResponse{DecodeHistoryReadEventsResponse(body)};
    if (service == "HistoryUpdate")
      return ServiceResponse{DecodeHistoryUpdateResponse(body)};
    if (service == "AddNodes")
      return ServiceResponse{DecodeAddNodesResponse(body)};
    if (service == "DeleteNodes") {
      ua::DeleteNodesResponse response;
      ua::DecodeJson(body, response);
      return ServiceResponse{std::move(response)};
    }
    if (service == "AddReferences") {
      ua::AddReferencesResponse response;
      ua::DecodeJson(body, response);
      return ServiceResponse{std::move(response)};
    }
    if (service == "DeleteReferences") {
      ua::DeleteReferencesResponse response;
      ua::DecodeJson(body, response);
      return ServiceResponse{std::move(response)};
    }
    return Status{StatusCode::Bad_TypeMismatch};
  } catch (...) {
    return Status{StatusCode::Bad_TypeMismatch};
  }
}

}  // namespace opcua::ws
