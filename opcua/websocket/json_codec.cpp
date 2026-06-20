#include "opcua/websocket/json_codec.h"

#include "opcua/base/time_utils.h"
#include "opcua/base/utf_convert.h"
#include "opcua/scada/standard_node_ids.h"
#include "opcua/scada/status.h"
#include "opcua/scada/variant.h"

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

value EncodeNodeId(const scada::NodeId& node_id) {
  return string(node_id.ToString());
}

scada::NodeId DecodeNodeId(const value& json) {
  auto node_id = scada::NodeId::FromString(RequireString(json));
  if (node_id.is_null() && RequireString(json) != "i=0")
    ThrowJsonError("Invalid NodeId");
  return node_id;
}

// OPC UA Part 6 §5.4.2.11: ExpandedNodeId is encoded as a JSON string using
// the textual form from §5.1.13 — the same NodeId text optionally prefixed
// with `nsu=<URI>;` (when NamespaceUri is set) and/or `svr=<index>;` (when
// ServerIndex is non-zero).
value EncodeExpandedNodeId(const scada::ExpandedNodeId& node_id) {
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

scada::ExpandedNodeId DecodeExpandedNodeId(const value& json) {
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
  auto node_id = scada::NodeId::FromString(text);
  if (node_id.is_null() && text != "i=0")
    ThrowJsonError("Invalid ExpandedNodeId");
  return {std::move(node_id), std::move(namespace_uri), server_index};
}

// OPC UA Part 6 §5.4.2.13: QualifiedName is an object with `Name` (string)
// and `Uri` (NamespaceIndex as a number; the spec name is `Uri` even though
// it carries an integer index). `Uri` is omitted when NamespaceIndex == 0.
value EncodeQualifiedName(const scada::QualifiedName& name) {
  object json{{"Name", name.name()}};
  if (name.namespace_index() != 0)
    json["Uri"] = name.namespace_index();
  return json;
}

scada::QualifiedName DecodeQualifiedName(const value& json) {
  const auto& obj = RequireObject(json);
  scada::NamespaceIndex namespace_index = 0;
  if (const auto* uri = FindField(obj, "Uri"))
    namespace_index = static_cast<scada::NamespaceIndex>(RequireUInt64(*uri));
  return {std::string{RequireString(RequireField(obj, "Name"))},
          namespace_index};
}

// OPC UA Part 6 §5.4.2.14: LocalizedText is an object with optional `Locale`
// and `Text` string fields, both omitted when null/empty. The scada
// LocalizedText carries text only (no locale), so we only ever emit `Text`.
value EncodeLocalizedText(const scada::LocalizedText& text) {
  object json;
  std::string utf8 = UtfConvert<char>(text);
  if (!utf8.empty())
    json["Text"] = std::move(utf8);
  return json;
}

scada::LocalizedText DecodeLocalizedText(const value& json) {
  const auto& obj = RequireObject(json);
  if (const auto* text = FindField(obj, "Text"))
    return UtfConvert<char16_t>(std::string{RequireString(*text)});
  return {};
}

value EncodeDateTime(scada::DateTime time) {
  if (time.is_null())
    return nullptr;
  base::Time::Exploded e = {};
  time.UTCExplode(&e);
  auto text = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", e.year,
                          e.month, e.day_of_month, e.hour, e.minute, e.second);
  if (e.millisecond != 0)
    text += std::format(".{:03}", e.millisecond);
  text += 'Z';
  return string(std::move(text));
}

scada::DateTime DecodeDateTime(const value& json) {
  if (json.is_null())
    return {};
  if (RequireString(json) == "0001-01-01T00:00:00Z")
    return {};
  scada::DateTime time;
  if (!Deserialize(RequireString(json), time))
    ThrowJsonError("Invalid date-time");
  return time;
}

value EncodeByteString(const scada::ByteString& bytes) {
  array result;
  result.reserve(bytes.size());
  for (char byte : bytes)
    result.emplace_back(
        static_cast<std::uint64_t>(static_cast<unsigned char>(byte)));
  return result;
}

scada::ByteString DecodeByteString(const value& json) {
  scada::ByteString bytes;
  for (const auto& entry : RequireArray(json)) {
    auto raw = RequireUInt64(entry);
    if (raw > std::numeric_limits<unsigned char>::max())
      ThrowJsonError("ByteString element out of range");
    bytes.push_back(static_cast<char>(static_cast<unsigned char>(raw)));
  }
  return bytes;
}

value EncodeExtensionObject(const scada::ExtensionObject& extension_object) {
  const auto* payload =
      std::any_cast<boost::json::value>(&extension_object.value());
  if (!payload)
    ThrowJsonError("Unsupported ExtensionObject payload");
  return object{
      {"TypeId", EncodeExpandedNodeId(extension_object.data_type_id())},
      {"Body", *payload}};
}

scada::ExtensionObject DecodeExtensionObject(const value& json) {
  const auto& obj = RequireObject(json);
  return {DecodeExpandedNodeId(RequireField(obj, "TypeId")),
          RequireField(obj, "Body")};
}

value EncodeStatus(const scada::Status& status) {
  return static_cast<std::uint64_t>(status.full_code());
}

scada::Status DecodeStatus(const value& json) {
  if (json.is_uint64() || (json.is_int64() && json.as_int64() >= 0)) {
    return scada::Status::FromFullCode(
        static_cast<unsigned>(RequireUInt64(json)));
  }
  const auto& obj = RequireObject(json);
  return scada::Status::FromFullCode(
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "fullCode"))));
}

value EncodeStatusCode(scada::StatusCode status_code) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(status_code));
}

scada::StatusCode DecodeStatusCode(const value& json) {
  return static_cast<scada::StatusCode>(
      static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeVariant(const scada::Variant& variant);
scada::Variant DecodeVariant(const value& json);

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
value EncodeDataValue(const scada::DataValue& data_value) {
  object json;
  if (data_value.value.type() != scada::Variant::EMPTY)
    json["Value"] = EncodeVariant(data_value.value);
  if (data_value.status_code != scada::StatusCode::Good)
    json["Status"] = EncodeStatusCode(data_value.status_code);
  if (!data_value.source_timestamp.is_null())
    json["SourceTimestamp"] = EncodeDateTime(data_value.source_timestamp);
  if (!data_value.server_timestamp.is_null())
    json["ServerTimestamp"] = EncodeDateTime(data_value.server_timestamp);
  return json;
}

scada::DataValue DecodeDataValue(const value& json) {
  const auto& obj = RequireObject(json);
  scada::DataValue result;
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

value EncodeEventFilter(const scada::EventFilter& filter) {
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

scada::EventFilter DecodeEventFilter(const value& json) {
  const auto& obj = RequireObject(json);
  scada::EventFilter filter;
  filter.types =
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "Types")));
  for (const auto& entry : RequireArray(RequireField(obj, "OfType")))
    filter.of_type.push_back(DecodeNodeId(entry));
  for (const auto& entry : RequireArray(RequireField(obj, "ChildOf")))
    filter.child_of.push_back(DecodeNodeId(entry));
  return filter;
}

value EncodeAggregateFilter(const scada::AggregateFilter& filter) {
  return object{{"StartTime", EncodeDateTime(filter.start_time)},
                {"Interval", filter.interval.InMicroseconds()},
                {"AggregateType", EncodeNodeId(filter.aggregate_type)}};
}

scada::AggregateFilter DecodeAggregateFilter(const value& json) {
  const auto& obj = RequireObject(json);
  return {.start_time = DecodeDateTime(RequireField(obj, "StartTime")),
          .interval = base::TimeDelta::FromMicroseconds(
              RequireInt64(RequireField(obj, "Interval"))),
          .aggregate_type = DecodeNodeId(RequireField(obj, "AggregateType"))};
}

value EncodeNodeClass(scada::NodeClass node_class) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(node_class));
}

scada::NodeClass DecodeNodeClass(const value& json) {
  return static_cast<scada::NodeClass>(
      static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeAttributeId(scada::AttributeId attribute_id) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(attribute_id));
}

scada::AttributeId DecodeAttributeId(const value& json) {
  return static_cast<scada::AttributeId>(
      static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeWriteFlags(scada::WriteFlags flags) {
  return static_cast<std::uint64_t>(flags.raw());
}

scada::WriteFlags DecodeWriteFlags(const value& json) {
  return scada::WriteFlags{static_cast<unsigned>(RequireUInt64(json))};
}

value EncodeBrowseDirection(scada::BrowseDirection direction) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(direction));
}

scada::BrowseDirection DecodeBrowseDirection(const value& json) {
  return static_cast<scada::BrowseDirection>(
      static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeNodeAttributes(const scada::NodeAttributes& attributes) {
  object json;
  if (!attributes.browse_name.empty())
    json["BrowseName"] = EncodeQualifiedName(attributes.browse_name);
  if (!attributes.display_name.empty())
    json["DisplayName"] = EncodeLocalizedText(attributes.display_name);
  if (!attributes.data_type.is_null())
    json["DataType"] = EncodeNodeId(attributes.data_type);
  if (attributes.value.has_value())
    json["Value"] = EncodeVariant(*attributes.value);
  return json;
}

scada::NodeAttributes DecodeNodeAttributes(const value& json) {
  const auto& obj = RequireObject(json);
  scada::NodeAttributes attributes;
  if (const auto* field = FindField(obj, "BrowseName"))
    attributes.browse_name = DecodeQualifiedName(*field);
  if (const auto* field = FindField(obj, "DisplayName"))
    attributes.display_name = DecodeLocalizedText(*field);
  if (const auto* field = FindField(obj, "DataType"))
    attributes.data_type = DecodeNodeId(*field);
  if (const auto* field = FindField(obj, "Value"))
    attributes.value = DecodeVariant(*field);
  return attributes;
}

value EncodeReadValueId(const scada::ReadValueId& value_id) {
  return object{{"NodeId", EncodeNodeId(value_id.node_id)},
                {"AttributeId", EncodeAttributeId(value_id.attribute_id)}};
}

scada::ReadValueId DecodeReadValueId(const value& json) {
  const auto& obj = RequireObject(json);
  return {.node_id = DecodeNodeId(RequireField(obj, "NodeId")),
          .attribute_id = DecodeAttributeId(RequireField(obj, "AttributeId"))};
}

value EncodeWriteValue(const scada::WriteValue& write_value) {
  return object{{"NodeId", EncodeNodeId(write_value.node_id)},
                {"AttributeId", EncodeAttributeId(write_value.attribute_id)},
                {"Value", EncodeVariant(write_value.value)},
                {"Flags", EncodeWriteFlags(write_value.flags)}};
}

scada::WriteValue DecodeWriteValue(const value& json) {
  const auto& obj = RequireObject(json);
  const auto& encoded_value = RequireField(obj, "Value");
  const auto* value_field = encoded_value.is_object()
                                ? FindField(encoded_value.as_object(), "Value")
                                : nullptr;
  return {.node_id = DecodeNodeId(RequireField(obj, "NodeId")),
          .attribute_id = DecodeAttributeId(RequireField(obj, "AttributeId")),
          // Accept both the spec/current bare Variant shape and the legacy
          // DataValue wrapper some web clients still send for Write.Value.
          .value = DecodeVariant(value_field ? *value_field : encoded_value),
          .flags = DecodeWriteFlags(RequireField(obj, "Flags"))};
}

value EncodeReferenceDescription(const scada::ReferenceDescription& reference) {
  // OPC UA Part 4, Browse Service Parameters, defines ResultMask bit 2 as
  // NodeClass, and Part 4 ReferenceDescription defines nodeClass as the target
  // Node's NodeClass:
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.9.2.2
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.29
  return object{{"ReferenceTypeId", EncodeNodeId(reference.reference_type_id)},
                {"Forward", reference.forward},
                {"NodeId", EncodeNodeId(reference.node_id)},
                {"NodeClass", EncodeNodeClass(reference.node_class)}};
}

scada::ReferenceDescription DecodeReferenceDescription(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .reference_type_id = DecodeNodeId(RequireField(obj, "ReferenceTypeId")),
      .forward = RequireBool(RequireField(obj, "Forward")),
      .node_id = DecodeNodeId(RequireField(obj, "NodeId")),
      .node_class = obj.contains("NodeClass")
                        ? DecodeNodeClass(RequireField(obj, "NodeClass"))
                        : scada::NodeClass::Unspecified};
}

value EncodeBrowseDescription(const scada::BrowseDescription& description) {
  return object{
      {"NodeId", EncodeNodeId(description.node_id)},
      {"Direction", EncodeBrowseDirection(description.direction)},
      {"ReferenceTypeId", EncodeNodeId(description.reference_type_id)},
      {"IncludeSubtypes", description.include_subtypes}};
}

scada::BrowseDescription DecodeBrowseDescription(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .node_id = DecodeNodeId(RequireField(obj, "NodeId")),
      .direction = DecodeBrowseDirection(RequireField(obj, "Direction")),
      .reference_type_id = DecodeNodeId(RequireField(obj, "ReferenceTypeId")),
      .include_subtypes = RequireBool(RequireField(obj, "IncludeSubtypes"))};
}

value EncodeBrowseResult(const scada::BrowseResult& result) {
  object json{{"StatusCode", EncodeStatusCode(result.status_code)},
              {"References",
               EncodeList(result.references, EncodeReferenceDescription)}};
  if (!result.continuation_point.empty())
    json["ContinuationPoint"] = EncodeByteString(result.continuation_point);
  return json;
}

scada::BrowseResult DecodeBrowseResult(const value& json) {
  const auto& obj = RequireObject(json);
  scada::BrowseResult result{
      .status_code = DecodeStatusCode(RequireField(obj, "StatusCode")),
      .references = DecodeList<scada::ReferenceDescription>(
          RequireField(obj, "References"), DecodeReferenceDescription)};
  if (const auto* continuation_point = FindField(obj, "ContinuationPoint")) {
    result.continuation_point = DecodeByteString(*continuation_point);
  }
  return result;
}

value EncodeRelativePathElement(
    const scada::RelativePathElement& path_element) {
  return object{
      {"ReferenceTypeId", EncodeNodeId(path_element.reference_type_id)},
      {"Inverse", path_element.inverse},
      {"IncludeSubtypes", path_element.include_subtypes},
      {"TargetName", EncodeQualifiedName(path_element.target_name)}};
}

scada::RelativePathElement DecodeRelativePathElement(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .reference_type_id = DecodeNodeId(RequireField(obj, "ReferenceTypeId")),
      .inverse = RequireBool(RequireField(obj, "Inverse")),
      .include_subtypes = RequireBool(RequireField(obj, "IncludeSubtypes")),
      .target_name = DecodeQualifiedName(RequireField(obj, "TargetName"))};
}

value EncodeBrowsePath(const scada::BrowsePath& path) {
  return object{{"NodeId", EncodeNodeId(path.node_id)},
                {"RelativePath",
                 EncodeList(path.relative_path, EncodeRelativePathElement)}};
}

scada::BrowsePath DecodeBrowsePath(const value& json) {
  const auto& obj = RequireObject(json);
  return {.node_id = DecodeNodeId(RequireField(obj, "NodeId")),
          .relative_path = DecodeList<scada::RelativePathElement>(
              RequireField(obj, "RelativePath"), DecodeRelativePathElement)};
}

value EncodeBrowsePathTarget(const scada::BrowsePathTarget& target) {
  return object{{"TargetId", EncodeExpandedNodeId(target.target_id)},
                {"RemainingPathIndex", target.remaining_path_index}};
}

scada::BrowsePathTarget DecodeBrowsePathTarget(const value& json) {
  const auto& obj = RequireObject(json);
  return {.target_id = DecodeExpandedNodeId(RequireField(obj, "TargetId")),
          .remaining_path_index = static_cast<size_t>(
              RequireUInt64(RequireField(obj, "RemainingPathIndex")))};
}

value EncodeBrowsePathResult(const scada::BrowsePathResult& result) {
  return object{
      {"StatusCode", EncodeStatusCode(result.status_code)},
      {"Targets", EncodeList(result.targets, EncodeBrowsePathTarget)}};
}

scada::BrowsePathResult DecodeBrowsePathResult(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status_code = DecodeStatusCode(RequireField(obj, "StatusCode")),
          .targets = DecodeList<scada::BrowsePathTarget>(
              RequireField(obj, "Targets"), DecodeBrowsePathTarget)};
}

value EncodeEvent(const scada::Event& event) {
  return object{
      {"EventTypeId", EncodeNodeId(event.event_type_id)},
      {"EventId", event.event_id},
      {"Time", EncodeDateTime(event.time)},
      {"ReceiveTime", EncodeDateTime(event.receive_time)},
      {"ChangeMask", event.change_mask},
      {"Severity", event.severity},
      {"NodeId", EncodeNodeId(event.node_id)},
      {"UserId", EncodeNodeId(event.user_id)},
      {"Value", EncodeVariant(event.value)},
      {"Qualifier", event.qualifier.raw()},
      {"Message", EncodeLocalizedText(event.message)},
      {"Acked", event.acked},
      {"AcknowledgedTime", EncodeDateTime(event.acknowledged_time)},
      {"AcknowledgedUserId", EncodeNodeId(event.acknowledged_user_id)},
  };
}

scada::Event DecodeEvent(const value& json) {
  const auto& obj = RequireObject(json);
  scada::Event event;
  event.event_type_id = DecodeNodeId(RequireField(obj, "EventTypeId"));
  event.event_id = RequireUInt64(RequireField(obj, "EventId"));
  event.time = DecodeDateTime(RequireField(obj, "Time"));
  event.receive_time = DecodeDateTime(RequireField(obj, "ReceiveTime"));
  event.change_mask = static_cast<scada::UInt32>(
      RequireUInt64(RequireField(obj, "ChangeMask")));
  event.severity =
      static_cast<scada::UInt32>(RequireUInt64(RequireField(obj, "Severity")));
  event.node_id = DecodeNodeId(RequireField(obj, "NodeId"));
  event.user_id = DecodeNodeId(RequireField(obj, "UserId"));
  event.value = DecodeVariant(RequireField(obj, "Value"));
  event.qualifier = scada::Qualifier{
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
unsigned BuiltInTypeId(scada::Variant::Type type) {
  switch (type) {
    case scada::Variant::EMPTY:
      return 0;
    case scada::Variant::BOOL:
      return 1;
    case scada::Variant::INT8:
      return 2;
    case scada::Variant::UINT8:
      return 3;
    case scada::Variant::INT16:
      return 4;
    case scada::Variant::UINT16:
      return 5;
    case scada::Variant::INT32:
      return 6;
    case scada::Variant::UINT32:
      return 7;
    case scada::Variant::INT64:
      return 8;
    case scada::Variant::UINT64:
      return 9;
    case scada::Variant::DOUBLE:
      return 11;
    case scada::Variant::STRING:
      return 12;
    case scada::Variant::DATE_TIME:
      return 13;
    case scada::Variant::BYTE_STRING:
      return 15;
    case scada::Variant::NODE_ID:
      return 17;
    case scada::Variant::EXPANDED_NODE_ID:
      return 18;
    case scada::Variant::QUALIFIED_NAME:
      return 20;
    case scada::Variant::LOCALIZED_TEXT:
      return 21;
    case scada::Variant::EXTENSION_OBJECT:
      return 22;
    case scada::Variant::COUNT:
      return 0;
  }
  return 0;
}

scada::Variant::Type FromBuiltInTypeId(unsigned id) {
  switch (id) {
    case 0:
      return scada::Variant::EMPTY;
    case 1:
      return scada::Variant::BOOL;
    case 2:
      return scada::Variant::INT8;
    case 3:
      return scada::Variant::UINT8;
    case 4:
      return scada::Variant::INT16;
    case 5:
      return scada::Variant::UINT16;
    case 6:
      return scada::Variant::INT32;
    case 7:
      return scada::Variant::UINT32;
    case 8:
      return scada::Variant::INT64;
    case 9:
      return scada::Variant::UINT64;
    case 11:
      return scada::Variant::DOUBLE;
    case 12:
      return scada::Variant::STRING;
    case 13:
      return scada::Variant::DATE_TIME;
    case 15:
      return scada::Variant::BYTE_STRING;
    case 17:
      return scada::Variant::NODE_ID;
    case 18:
      return scada::Variant::EXPANDED_NODE_ID;
    case 20:
      return scada::Variant::QUALIFIED_NAME;
    case 21:
      return scada::Variant::LOCALIZED_TEXT;
    case 22:
      return scada::Variant::EXTENSION_OBJECT;
    default:
      return scada::Variant::COUNT;
  }
}
}  // namespace

value EncodeVariant(const scada::Variant& variant) {
  if (variant.type() == scada::Variant::EMPTY)
    return nullptr;

  object json{{"Type", BuiltInTypeId(variant.type())}};

  if (variant.is_scalar()) {
    switch (variant.type()) {
      case scada::Variant::EMPTY:
        break;
      case scada::Variant::BOOL:
        json["Body"] = variant.get<bool>();
        break;
      case scada::Variant::INT8:
        json["Body"] = variant.get<scada::Int8>();
        break;
      case scada::Variant::UINT8:
        json["Body"] = variant.get<scada::UInt8>();
        break;
      case scada::Variant::INT16:
        json["Body"] = variant.get<scada::Int16>();
        break;
      case scada::Variant::UINT16:
        json["Body"] = variant.get<scada::UInt16>();
        break;
      case scada::Variant::INT32:
        json["Body"] = variant.get<scada::Int32>();
        break;
      case scada::Variant::UINT32:
        json["Body"] = variant.get<scada::UInt32>();
        break;
      case scada::Variant::INT64:
        json["Body"] = variant.get<scada::Int64>();
        break;
      case scada::Variant::UINT64:
        json["Body"] = variant.get<scada::UInt64>();
        break;
      case scada::Variant::DOUBLE:
        json["Body"] = variant.get<double>();
        break;
      case scada::Variant::BYTE_STRING:
        json["Body"] = EncodeByteString(variant.get<scada::ByteString>());
        break;
      case scada::Variant::STRING:
        json["Body"] = variant.get<scada::String>();
        break;
      case scada::Variant::QUALIFIED_NAME:
        json["Body"] = EncodeQualifiedName(variant.get<scada::QualifiedName>());
        break;
      case scada::Variant::LOCALIZED_TEXT:
        json["Body"] = EncodeLocalizedText(variant.get<scada::LocalizedText>());
        break;
      case scada::Variant::NODE_ID:
        json["Body"] = EncodeNodeId(variant.get<scada::NodeId>());
        break;
      case scada::Variant::EXPANDED_NODE_ID:
        json["Body"] =
            EncodeExpandedNodeId(variant.get<scada::ExpandedNodeId>());
        break;
      case scada::Variant::EXTENSION_OBJECT:
        json["Body"] =
            EncodeExtensionObject(variant.get<scada::ExtensionObject>());
        break;
      case scada::Variant::DATE_TIME:
        json["Body"] = EncodeDateTime(variant.get<scada::DateTime>());
        break;
      case scada::Variant::COUNT:
        ThrowJsonError("Unexpected scalar variant type");
    }
  } else {
    switch (variant.type()) {
      case scada::Variant::EMPTY:
        json["Body"] =
            array(variant.get<std::vector<std::monostate>>().size(), nullptr);
        break;
      case scada::Variant::BOOL:
        json["Body"] = EncodeScalarList(variant.get<std::vector<bool>>());
        break;
      case scada::Variant::INT8:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::Int8>>());
        break;
      case scada::Variant::UINT8:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::UInt8>>());
        break;
      case scada::Variant::INT16:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::Int16>>());
        break;
      case scada::Variant::UINT16:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::UInt16>>());
        break;
      case scada::Variant::INT32:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::Int32>>());
        break;
      case scada::Variant::UINT32:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::UInt32>>());
        break;
      case scada::Variant::INT64:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::Int64>>());
        break;
      case scada::Variant::UINT64:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::UInt64>>());
        break;
      case scada::Variant::DOUBLE:
        json["Body"] = EncodeScalarList(variant.get<std::vector<double>>());
        break;
      case scada::Variant::BYTE_STRING:
        json["Body"] = EncodeList(variant.get<std::vector<scada::ByteString>>(),
                                  EncodeByteString);
        break;
      case scada::Variant::STRING:
        json["Body"] =
            EncodeScalarList(variant.get<std::vector<scada::String>>());
        break;
      case scada::Variant::QUALIFIED_NAME:
        json["Body"] =
            EncodeList(variant.get<std::vector<scada::QualifiedName>>(),
                       EncodeQualifiedName);
        break;
      case scada::Variant::LOCALIZED_TEXT:
        json["Body"] =
            EncodeList(variant.get<std::vector<scada::LocalizedText>>(),
                       EncodeLocalizedText);
        break;
      case scada::Variant::NODE_ID:
        json["Body"] =
            EncodeList(variant.get<std::vector<scada::NodeId>>(), EncodeNodeId);
        break;
      case scada::Variant::EXPANDED_NODE_ID:
        json["Body"] =
            EncodeList(variant.get<std::vector<scada::ExpandedNodeId>>(),
                       EncodeExpandedNodeId);
        break;
      case scada::Variant::DATE_TIME:
        ThrowJsonError("DateTime array codec not implemented");
      case scada::Variant::EXTENSION_OBJECT:
        json["Body"] =
            EncodeList(variant.get<std::vector<scada::ExtensionObject>>(),
                       EncodeExtensionObject);
        break;
      case scada::Variant::COUNT:
        ThrowJsonError("Unexpected array variant type");
    }
  }

  return json;
}

scada::Variant DecodeVariant(const value& json) {
  // Spec: a Variant carrying Type 0 (Null) is encoded as the JSON literal
  // null with no Body. Treat top-level null as an empty Variant.
  if (json.is_null())
    return {};

  const auto& obj = RequireObject(json);
  auto type = FromBuiltInTypeId(
      static_cast<unsigned>(RequireUInt64(RequireField(obj, "Type"))));
  if (type == scada::Variant::COUNT)
    ThrowJsonError("Unsupported Variant Type id");

  // No `IsArray` on the wire — derived from the Body's JSON kind.
  const auto* body_field = FindField(obj, "Body");
  if (body_field == nullptr || body_field->is_null())
    return {};
  const auto& payload = *body_field;
  bool is_array = payload.is_array();

  if (!is_array) {
    switch (type) {
      case scada::Variant::EMPTY:
        return {};
      case scada::Variant::BOOL:
        return scada::Variant{RequireBool(payload)};
      case scada::Variant::INT8:
        return scada::Variant{static_cast<scada::Int8>(RequireInt64(payload))};
      case scada::Variant::UINT8:
        return scada::Variant{
            static_cast<scada::UInt8>(RequireUInt64(payload))};
      case scada::Variant::INT16:
        return scada::Variant{static_cast<scada::Int16>(RequireInt64(payload))};
      case scada::Variant::UINT16:
        return scada::Variant{
            static_cast<scada::UInt16>(RequireUInt64(payload))};
      case scada::Variant::INT32:
        return scada::Variant{static_cast<scada::Int32>(RequireInt64(payload))};
      case scada::Variant::UINT32:
        return scada::Variant{
            static_cast<scada::UInt32>(RequireUInt64(payload))};
      case scada::Variant::INT64:
        return scada::Variant{static_cast<scada::Int64>(RequireInt64(payload))};
      case scada::Variant::UINT64:
        return scada::Variant{
            static_cast<scada::UInt64>(RequireUInt64(payload))};
      case scada::Variant::DOUBLE:
        return scada::Variant{RequireDouble(payload)};
      case scada::Variant::BYTE_STRING:
        return scada::Variant{DecodeByteString(payload)};
      case scada::Variant::STRING:
        return scada::Variant{std::string{RequireString(payload)}};
      case scada::Variant::QUALIFIED_NAME:
        return scada::Variant{DecodeQualifiedName(payload)};
      case scada::Variant::LOCALIZED_TEXT:
        return scada::Variant{DecodeLocalizedText(payload)};
      case scada::Variant::NODE_ID:
        return scada::Variant{DecodeNodeId(payload)};
      case scada::Variant::EXPANDED_NODE_ID:
        return scada::Variant{DecodeExpandedNodeId(payload)};
      case scada::Variant::DATE_TIME:
        return scada::Variant{DecodeDateTime(payload)};
      case scada::Variant::EXTENSION_OBJECT:
        return scada::Variant{DecodeExtensionObject(payload)};
      case scada::Variant::COUNT:
        ThrowJsonError("Unsupported scalar variant type");
    }
  }

  switch (type) {
    case scada::Variant::EMPTY:
      return scada::Variant{DecodeList<std::monostate>(
          payload, [](const value&) { return std::monostate{}; })};
    case scada::Variant::BOOL:
      return scada::Variant{DecodeList<bool>(payload, RequireBool)};
    case scada::Variant::INT8:
      return scada::Variant{DecodeIntList<scada::Int8>(payload)};
    case scada::Variant::UINT8:
      return scada::Variant{DecodeUIntList<scada::UInt8>(payload)};
    case scada::Variant::INT16:
      return scada::Variant{DecodeIntList<scada::Int16>(payload)};
    case scada::Variant::UINT16:
      return scada::Variant{DecodeUIntList<scada::UInt16>(payload)};
    case scada::Variant::INT32:
      return scada::Variant{DecodeIntList<scada::Int32>(payload)};
    case scada::Variant::UINT32:
      return scada::Variant{DecodeUIntList<scada::UInt32>(payload)};
    case scada::Variant::INT64:
      return scada::Variant{DecodeIntList<scada::Int64>(payload)};
    case scada::Variant::UINT64:
      return scada::Variant{DecodeUIntList<scada::UInt64>(payload)};
    case scada::Variant::DOUBLE:
      return scada::Variant{DecodeList<double>(payload, RequireDouble)};
    case scada::Variant::BYTE_STRING:
      return scada::Variant{
          DecodeList<scada::ByteString>(payload, DecodeByteString)};
    case scada::Variant::STRING:
      return scada::Variant{DecodeList<scada::String>(
          payload,
          [](const value& v) { return std::string{RequireString(v)}; })};
    case scada::Variant::QUALIFIED_NAME:
      return scada::Variant{
          DecodeList<scada::QualifiedName>(payload, DecodeQualifiedName)};
    case scada::Variant::LOCALIZED_TEXT:
      return scada::Variant{
          DecodeList<scada::LocalizedText>(payload, DecodeLocalizedText)};
    case scada::Variant::NODE_ID:
      return scada::Variant{DecodeList<scada::NodeId>(payload, DecodeNodeId)};
    case scada::Variant::EXPANDED_NODE_ID:
      return scada::Variant{
          DecodeList<scada::ExpandedNodeId>(payload, DecodeExpandedNodeId)};
    case scada::Variant::DATE_TIME:
      ThrowJsonError("Unsupported array variant type");
    case scada::Variant::EXTENSION_OBJECT:
      return scada::Variant{
          DecodeList<scada::ExtensionObject>(payload, DecodeExtensionObject)};
    case scada::Variant::COUNT:
      ThrowJsonError("Unsupported array variant type");
  }

  ThrowJsonError("Unsupported variant type");
}

value EncodeReadRequest(const ReadRequest& request) {
  return object{{"NodesToRead", EncodeList(request.inputs, EncodeReadValueId)}};
}

ReadRequest DecodeReadRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "NodesToRead");
  if (!field)
    field = FindField(obj, "Inputs");
  if (!field)
    ThrowJsonError("Missing NodesToRead");
  return {.inputs = DecodeList<scada::ReadValueId>(*field, DecodeReadValueId)};
}

value EncodeWriteRequest(const WriteRequest& request) {
  return object{{"NodesToWrite", EncodeList(request.inputs, EncodeWriteValue)}};
}

WriteRequest DecodeWriteRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "NodesToWrite");
  if (!field)
    field = FindField(obj, "Inputs");
  if (!field)
    ThrowJsonError("Missing NodesToWrite");
  return {.inputs = DecodeList<scada::WriteValue>(*field, DecodeWriteValue)};
}

value EncodeBrowseRequest(const BrowseRequest& request) {
  return object{
      {"RequestedMaxReferencesPerNode",
       request.requested_max_references_per_node},
      {"NodesToBrowse", EncodeList(request.inputs, EncodeBrowseDescription)}};
}

BrowseRequest DecodeBrowseRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "NodesToBrowse");
  if (!field)
    field = FindField(obj, "Inputs");
  if (!field)
    ThrowJsonError("Missing NodesToBrowse");
  return {
      .requested_max_references_per_node = static_cast<size_t>(
          RequireUInt64(RequireField(obj, "RequestedMaxReferencesPerNode"))),
      .inputs = DecodeList<scada::BrowseDescription>(*field,
                                                     DecodeBrowseDescription)};
}

value EncodeBrowseNextRequest(const BrowseNextRequest& request) {
  return object{
      {"ReleaseContinuationPoints", request.release_continuation_points},
      {"ContinuationPoints",
       EncodeList(request.continuation_points, EncodeByteString)}};
}

BrowseNextRequest DecodeBrowseNextRequest(const value& json) {
  const auto& obj = RequireObject(json);
  return {.release_continuation_points =
              RequireBool(RequireField(obj, "ReleaseContinuationPoints")),
          .continuation_points = DecodeList<scada::ByteString>(
              RequireField(obj, "ContinuationPoints"), DecodeByteString)};
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
  return {.inputs = DecodeList<scada::BrowsePath>(*field, DecodeBrowsePath)};
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

value EncodeCallRequest(const CallRequest& request) {
  return object{
      {"MethodsToCall",
       EncodeList(request.methods, [](const MethodCallRequest& method) {
         return object{
             {"ObjectId", EncodeNodeId(method.object_id)},
             {"MethodId", EncodeNodeId(method.method_id)},
             {"InputArguments", EncodeList(method.arguments, EncodeVariant)}};
       })}};
}

CallRequest DecodeCallRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* methods = FindField(obj, "MethodsToCall");
  if (!methods)
    methods = FindField(obj, "Methods");
  if (!methods)
    ThrowJsonError("Missing MethodsToCall");
  return {.methods =
              DecodeList<MethodCallRequest>(*methods, [](const value& entry) {
                const auto& obj = RequireObject(entry);
                const auto* arguments = FindField(obj, "InputArguments");
                if (!arguments)
                  arguments = FindField(obj, "Arguments");
                if (!arguments)
                  ThrowJsonError("Missing InputArguments");
                return MethodCallRequest{
                    .object_id = DecodeNodeId(RequireField(obj, "ObjectId")),
                    .method_id = DecodeNodeId(RequireField(obj, "MethodId")),
                    .arguments =
                        DecodeList<scada::Variant>(*arguments, DecodeVariant)};
              })};
}

value EncodeAddNodesRequest(const AddNodesRequest& request) {
  return object{
      {"NodesToAdd",
       EncodeList(request.items, [](const scada::AddNodesItem& item) {
         return object{
             {"RequestedId", EncodeNodeId(item.requested_id)},
             {"ParentId", EncodeNodeId(item.parent_id)},
             {"NodeClass", EncodeNodeClass(item.node_class)},
             {"TypeDefinitionId", EncodeNodeId(item.type_definition_id)},
             {"Attributes", EncodeNodeAttributes(item.attributes)}};
       })}};
}

AddNodesRequest DecodeAddNodesRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "NodesToAdd");
  if (!field)
    field = FindField(obj, "Items");
  if (!field)
    ThrowJsonError("Missing NodesToAdd");
  return {
      .items = DecodeList<scada::AddNodesItem>(*field, [](const value& entry) {
        const auto& obj = RequireObject(entry);
        return scada::AddNodesItem{
            .requested_id = DecodeNodeId(RequireField(obj, "RequestedId")),
            .parent_id = DecodeNodeId(RequireField(obj, "ParentId")),
            .node_class = DecodeNodeClass(RequireField(obj, "NodeClass")),
            .type_definition_id =
                DecodeNodeId(RequireField(obj, "TypeDefinitionId")),
            .attributes =
                DecodeNodeAttributes(RequireField(obj, "Attributes"))};
      })};
}

value EncodeDeleteNodesRequest(const DeleteNodesRequest& request) {
  return object{
      {"NodesToDelete",
       EncodeList(request.items, [](const scada::DeleteNodesItem& item) {
         return object{
             {"NodeId", EncodeNodeId(item.node_id)},
             {"DeleteTargetReferences", item.delete_target_references}};
       })}};
}

DeleteNodesRequest DecodeDeleteNodesRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "NodesToDelete");
  if (!field)
    field = FindField(obj, "Items");
  if (!field)
    ThrowJsonError("Missing NodesToDelete");
  return {.items = DecodeList<scada::DeleteNodesItem>(
              *field, [](const value& entry) {
                const auto& obj = RequireObject(entry);
                return scada::DeleteNodesItem{
                    .node_id = DecodeNodeId(RequireField(obj, "NodeId")),
                    .delete_target_references = RequireBool(
                        RequireField(obj, "DeleteTargetReferences"))};
              })};
}

value EncodeAddReferencesRequest(const AddReferencesRequest& request) {
  return object{
      {"ReferencesToAdd",
       EncodeList(request.items, [](const scada::AddReferencesItem& item) {
         return object{
             {"SourceNodeId", EncodeNodeId(item.source_node_id)},
             {"ReferenceTypeId", EncodeNodeId(item.reference_type_id)},
             {"IsForward", item.forward},
             {"TargetServerUri", item.target_server_uri},
             {"TargetNodeId", EncodeExpandedNodeId(item.target_node_id)},
             {"TargetNodeClass", EncodeNodeClass(item.target_node_class)}};
       })}};
}

AddReferencesRequest DecodeAddReferencesRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "ReferencesToAdd");
  if (!field)
    field = FindField(obj, "Items");
  if (!field)
    ThrowJsonError("Missing ReferencesToAdd");
  return {.items = DecodeList<scada::AddReferencesItem>(
              *field, [](const value& entry) {
                const auto& obj = RequireObject(entry);
                const auto* is_forward = FindField(obj, "IsForward");
                if (!is_forward)
                  is_forward = FindField(obj, "Forward");
                if (!is_forward)
                  ThrowJsonError("Missing IsForward");
                return scada::AddReferencesItem{
                    .source_node_id =
                        DecodeNodeId(RequireField(obj, "SourceNodeId")),
                    .reference_type_id =
                        DecodeNodeId(RequireField(obj, "ReferenceTypeId")),
                    .forward = RequireBool(*is_forward),
                    .target_server_uri = std::string{RequireString(
                        RequireField(obj, "TargetServerUri"))},
                    .target_node_id =
                        DecodeExpandedNodeId(RequireField(obj, "TargetNodeId")),
                    .target_node_class =
                        DecodeNodeClass(RequireField(obj, "TargetNodeClass"))};
              })};
}

value EncodeDeleteReferencesRequest(const DeleteReferencesRequest& request) {
  return object{
      {"ReferencesToDelete",
       EncodeList(request.items, [](const scada::DeleteReferencesItem& item) {
         return object{
             {"SourceNodeId", EncodeNodeId(item.source_node_id)},
             {"ReferenceTypeId", EncodeNodeId(item.reference_type_id)},
             {"IsForward", item.forward},
             {"TargetNodeId", EncodeExpandedNodeId(item.target_node_id)},
             {"DeleteBidirectional", item.delete_bidirectional}};
       })}};
}

DeleteReferencesRequest DecodeDeleteReferencesRequest(const value& json) {
  const auto& obj = RequireObject(json);
  const auto* field = FindField(obj, "ReferencesToDelete");
  if (!field)
    field = FindField(obj, "Items");
  if (!field)
    ThrowJsonError("Missing ReferencesToDelete");
  return {.items = DecodeList<scada::DeleteReferencesItem>(
              *field, [](const value& entry) {
                const auto& obj = RequireObject(entry);
                const auto* is_forward = FindField(obj, "IsForward");
                if (!is_forward)
                  is_forward = FindField(obj, "Forward");
                if (!is_forward)
                  ThrowJsonError("Missing IsForward");
                return scada::DeleteReferencesItem{
                    .source_node_id =
                        DecodeNodeId(RequireField(obj, "SourceNodeId")),
                    .reference_type_id =
                        DecodeNodeId(RequireField(obj, "ReferenceTypeId")),
                    .forward = RequireBool(*is_forward),
                    .target_node_id =
                        DecodeExpandedNodeId(RequireField(obj, "TargetNodeId")),
                    .delete_bidirectional =
                        RequireBool(RequireField(obj, "DeleteBidirectional"))};
              })};
}

template <class Response>
value EncodeDataValueResponse(const Response& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"Results", EncodeList(response.results, EncodeDataValue)}};
}

template <class Response>
Response DecodeDataValueResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<scada::DataValue>(RequireField(obj, "Results"),
                                                  DecodeDataValue)};
}

value EncodeBrowseResponse(const BrowseResponse& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"Results", EncodeList(response.results, EncodeBrowseResult)}};
}

BrowseResponse DecodeBrowseResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<scada::BrowseResult>(
              RequireField(obj, "Results"), DecodeBrowseResult)};
}

value EncodeBrowseNextResponse(const BrowseNextResponse& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"Results", EncodeList(response.results, EncodeBrowseResult)}};
}

BrowseNextResponse DecodeBrowseNextResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<scada::BrowseResult>(
              RequireField(obj, "Results"), DecodeBrowseResult)};
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
          .results = DecodeList<scada::BrowsePathResult>(
              RequireField(obj, "Results"), DecodeBrowsePathResult)};
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
                     .values = DecodeList<scada::DataValue>(
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
                     .events = DecodeList<scada::Event>(
                         RequireField(result, "Events"), DecodeEvent)}};
}

value EncodeCallResponse(const CallResponse& response) {
  return object{
      {"Results",
       EncodeList(response.results, [](const MethodCallResult& result) {
         return object{
             {"StatusCode", EncodeStatus(result.status)},
             {"InputArgumentResults",
              EncodeList(result.input_argument_results, EncodeStatusCode)},
             {"OutputArguments",
              EncodeList(result.output_arguments, EncodeVariant)}};
       })}};
}

CallResponse DecodeCallResponse(const value& json) {
  return {
      .results = DecodeList<MethodCallResult>(
          RequireField(RequireObject(json), "Results"), [](const value& entry) {
            const auto& obj = RequireObject(entry);
            const auto* status = FindField(obj, "StatusCode");
            if (!status)
              status = FindField(obj, "Status");
            if (!status)
              ThrowJsonError("Missing StatusCode");
            return MethodCallResult{
                .status = DecodeStatus(*status),
                .input_argument_results = DecodeList<scada::StatusCode>(
                    FindField(obj, "InputArgumentResults")
                        ? *FindField(obj, "InputArgumentResults")
                        : value{array{}},
                    DecodeStatusCode),
                .output_arguments = DecodeList<scada::Variant>(
                    FindField(obj, "OutputArguments")
                        ? *FindField(obj, "OutputArguments")
                        : value{array{}},
                    DecodeVariant)};
          })};
}

value EncodeAddNodesResponse(const AddNodesResponse& response) {
  return object{
      {"Status", EncodeStatus(response.status)},
      {"Results",
       EncodeList(response.results, [](const scada::AddNodesResult& result) {
         return object{{"StatusCode", EncodeStatusCode(result.status_code)},
                       {"AddedNodeId", EncodeNodeId(result.added_node_id)}};
       })}};
}

AddNodesResponse DecodeAddNodesResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<scada::AddNodesResult>(
              RequireField(obj, "Results"), [](const value& entry) {
                const auto& result = RequireObject(entry);
                return scada::AddNodesResult{
                    .status_code =
                        DecodeStatusCode(RequireField(result, "StatusCode")),
                    .added_node_id =
                        DecodeNodeId(RequireField(result, "AddedNodeId"))};
              })};
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
          .results = DecodeList<scada::StatusCode>(RequireField(obj, "Results"),
                                                   DecodeStatusCode)};
}

template <class T>
constexpr std::string_view RequestServiceName();

template <>
constexpr std::string_view RequestServiceName<ReadRequest>() {
  return "Read";
}
template <>
constexpr std::string_view RequestServiceName<WriteRequest>() {
  return "Write";
}
template <>
constexpr std::string_view RequestServiceName<BrowseRequest>() {
  return "Browse";
}
template <>
constexpr std::string_view RequestServiceName<BrowseNextRequest>() {
  return "BrowseNext";
}
template <>
constexpr std::string_view RequestServiceName<TranslateBrowsePathsRequest>() {
  return "TranslateBrowsePathsToNodeIds";
}
template <>
constexpr std::string_view RequestServiceName<CallRequest>() {
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
constexpr std::string_view RequestServiceName<AddNodesRequest>() {
  return "AddNodes";
}
template <>
constexpr std::string_view RequestServiceName<DeleteNodesRequest>() {
  return "DeleteNodes";
}
template <>
constexpr std::string_view RequestServiceName<AddReferencesRequest>() {
  return "AddReferences";
}
template <>
constexpr std::string_view RequestServiceName<DeleteReferencesRequest>() {
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
                                     ReadRequest>) {
          json["body"] = EncodeReadRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 WriteRequest>) {
          json["body"] = EncodeWriteRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 BrowseRequest>) {
          json["body"] = EncodeBrowseRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 BrowseNextRequest>) {
          json["body"] = EncodeBrowseNextRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 TranslateBrowsePathsRequest>) {
          json["body"] = EncodeTranslateBrowsePathsRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 CallRequest>) {
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
                                 AddNodesRequest>) {
          json["body"] = EncodeAddNodesRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 DeleteNodesRequest>) {
          json["body"] = EncodeDeleteNodesRequest(typed_request);
        } else if constexpr (std::is_same_v<
                                 std::decay_t<decltype(typed_request)>,
                                 AddReferencesRequest>) {
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
        if constexpr (std::is_same_v<T, ReadResponse>) {
          json["service"] = "Read";
          json["body"] = EncodeDataValueResponse(typed_response);
        } else if constexpr (std::is_same_v<T, WriteResponse>) {
          json["service"] = "Write";
          json["body"] = EncodeMultiStatusResponse(typed_response);
        } else if constexpr (std::is_same_v<T, BrowseResponse>) {
          json["service"] = "Browse";
          json["body"] = EncodeBrowseResponse(typed_response);
        } else if constexpr (std::is_same_v<T, BrowseNextResponse>) {
          json["service"] = "BrowseNext";
          json["body"] = EncodeBrowseNextResponse(typed_response);
        } else if constexpr (std::is_same_v<T, TranslateBrowsePathsResponse>) {
          json["service"] = "TranslateBrowsePathsToNodeIds";
          json["body"] = EncodeTranslateBrowsePathsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, CallResponse>) {
          json["service"] = "Call";
          json["body"] = EncodeCallResponse(typed_response);
        } else if constexpr (std::is_same_v<T, HistoryReadRawResponse>) {
          json["service"] = "HistoryReadRaw";
          json["body"] = EncodeHistoryReadRawResponse(typed_response);
        } else if constexpr (std::is_same_v<T, HistoryReadEventsResponse>) {
          json["service"] = "HistoryReadEvents";
          json["body"] = EncodeHistoryReadEventsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, AddNodesResponse>) {
          json["service"] = "AddNodes";
          json["body"] = EncodeAddNodesResponse(typed_response);
        } else if constexpr (std::is_same_v<T, DeleteNodesResponse>) {
          json["service"] = "DeleteNodes";
          json["body"] = EncodeMultiStatusResponse(typed_response);
        } else if constexpr (std::is_same_v<T, AddReferencesResponse>) {
          json["service"] = "AddReferences";
          json["body"] = EncodeMultiStatusResponse(typed_response);
        } else {
          json["service"] = "DeleteReferences";
          json["body"] = EncodeMultiStatusResponse(typed_response);
        }
        return json;
      },
      response);
}

scada::StatusOr<ServiceRequest> DecodeServiceRequest(
    const boost::json::value& json) {
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
    if (service == "AddNodes")
      return ServiceRequest{DecodeAddNodesRequest(body)};
    if (service == "DeleteNodes")
      return ServiceRequest{DecodeDeleteNodesRequest(body)};
    if (service == "AddReferences")
      return ServiceRequest{DecodeAddReferencesRequest(body)};
    if (service == "DeleteReferences")
      return ServiceRequest{DecodeDeleteReferencesRequest(body)};
    return scada::Status{scada::StatusCode::Bad_CantParseString};
  } catch (...) {
    return scada::Status{scada::StatusCode::Bad_CantParseString};
  }
}

scada::StatusOr<ServiceResponse> DecodeServiceResponse(
    const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    auto service = RequireString(RequireField(obj, "service"));
    if (service == "Read")
      return ServiceResponse{DecodeDataValueResponse<ReadResponse>(body)};
    if (service == "Write")
      return ServiceResponse{DecodeMultiStatusResponse<WriteResponse>(body)};
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
    if (service == "AddNodes")
      return ServiceResponse{DecodeAddNodesResponse(body)};
    if (service == "DeleteNodes")
      return ServiceResponse{
          DecodeMultiStatusResponse<DeleteNodesResponse>(body)};
    if (service == "AddReferences")
      return ServiceResponse{
          DecodeMultiStatusResponse<AddReferencesResponse>(body)};
    if (service == "DeleteReferences")
      return ServiceResponse{
          DecodeMultiStatusResponse<DeleteReferencesResponse>(body)};
    return scada::Status{scada::StatusCode::Bad_CantParseString};
  } catch (...) {
    return scada::Status{scada::StatusCode::Bad_CantParseString};
  }
}

}  // namespace opcua::ws
