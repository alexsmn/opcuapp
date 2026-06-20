#include "opcua/websocket/json_codec.h"

#include <boost/json.hpp>

#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace opcua::ws::detail {

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

const value& RequireField(const object& json, std::string_view key) {
  if (const auto* field = json.if_contains(key))
    return *field;
  ThrowJsonError("Missing required field");
}

const value* FindField(const object& json, std::string_view key) {
  return json.if_contains(key);
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

value EncodeNodeId(const scada::NodeId& node_id) {
  return string(node_id.ToString());
}

scada::NodeId DecodeNodeId(const value& json) {
  const auto text = RequireString(json);
  auto node_id = scada::NodeId::FromString(text);
  if (node_id.is_null() && text != "i=0")
    ThrowJsonError("Invalid NodeId");
  return node_id;
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
  return scada::Status::FromFullCode(static_cast<unsigned>(
      RequireUInt64(RequireField(obj, "fullCode"))));
}

value EncodeStatusCode(scada::StatusCode status_code) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(status_code));
}

scada::StatusCode DecodeStatusCode(const value& json) {
  return static_cast<scada::StatusCode>(
      static_cast<unsigned>(RequireUInt64(json)));
}

value EncodeAttributeId(scada::AttributeId attribute_id) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(attribute_id));
}

scada::AttributeId DecodeAttributeId(const value& json) {
  return static_cast<scada::AttributeId>(
      static_cast<unsigned>(RequireUInt64(json)));
}

template <class T, class Encoder>
array EncodeList(const std::vector<T>& values, Encoder&& encoder) {
  array result;
  result.reserve(values.size());
  for (const auto& entry : values)
    result.emplace_back(encoder(entry));
  return result;
}

template <class T, class Decoder>
std::vector<T> DecodeList(const value& json, Decoder&& decoder) {
  std::vector<T> result;
  for (const auto& entry : RequireArray(json))
    result.emplace_back(decoder(entry));
  return result;
}

template <class Enum>
value EncodeEnum(Enum value) {
  return static_cast<std::uint64_t>(
      static_cast<std::underlying_type_t<Enum>>(value));
}

template <class Enum>
Enum DecodeEnum(const value& json) {
  return static_cast<Enum>(
      static_cast<std::underlying_type_t<Enum>>(RequireUInt64(json)));
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

value EncodeDataChangeFilter(const DataChangeFilter& filter) {
  return object{{"FilterType", "DataChange"},
                {"Trigger", EncodeEnum(filter.trigger)},
                {"DeadbandType", EncodeEnum(filter.deadband_type)},
                {"DeadbandValue", filter.deadband_value}};
}

DataChangeFilter DecodeDataChangeFilter(const value& json) {
  const auto& obj = RequireObject(json);
  return {.trigger = DecodeEnum<DataChangeTrigger>(
              RequireField(obj, "Trigger")),
          .deadband_type = DecodeEnum<DeadbandType>(
              RequireField(obj, "DeadbandType")),
          .deadband_value = RequireDouble(RequireField(obj, "DeadbandValue"))};
}

value EncodeMonitoringFilter(const MonitoringFilter& filter) {
  if (const auto* data_change = std::get_if<DataChangeFilter>(&filter))
    return EncodeDataChangeFilter(*data_change);
  return std::get<boost::json::value>(filter);
}

MonitoringFilter DecodeMonitoringFilter(const value& json) {
  if (json.is_object()) {
    const auto& obj = json.as_object();
    if (const auto* field = FindField(obj, "FilterType");
        field != nullptr && RequireString(*field) == "DataChange") {
      return MonitoringFilter{DecodeDataChangeFilter(json)};
    }
  }
  return MonitoringFilter{json};
}

value EncodeMonitoringParameters(
    const MonitoringParameters& parameters) {
  object json{{"ClientHandle", parameters.client_handle},
              {"SamplingInterval", parameters.sampling_interval_ms},
              {"QueueSize", parameters.queue_size},
              {"DiscardOldest", parameters.discard_oldest}};
  if (parameters.filter.has_value())
    json["Filter"] = EncodeMonitoringFilter(*parameters.filter);
  return json;
}

MonitoringParameters DecodeMonitoringParameters(const value& json) {
  const auto& obj = RequireObject(json);
  MonitoringParameters parameters{
      .client_handle =
          static_cast<scada::UInt32>(RequireUInt64(RequireField(obj, "ClientHandle"))),
      .sampling_interval_ms =
          RequireDouble(RequireField(obj, "SamplingInterval")),
      .queue_size =
          static_cast<scada::UInt32>(RequireUInt64(RequireField(obj, "QueueSize"))),
      .discard_oldest = RequireBool(RequireField(obj, "DiscardOldest")),
  };
  if (const auto* field = FindField(obj, "Filter"))
    parameters.filter = DecodeMonitoringFilter(*field);
  return parameters;
}

value EncodeMonitoredItemCreateRequest(
    const MonitoredItemCreateRequest& request) {
  object json{{"ItemToMonitor", EncodeReadValueId(request.item_to_monitor)},
              {"MonitoringMode", EncodeEnum(request.monitoring_mode)},
              {"RequestedParameters",
               EncodeMonitoringParameters(request.requested_parameters)}};
  if (request.index_range.has_value())
    json["IndexRange"] = *request.index_range;
  return json;
}

MonitoredItemCreateRequest DecodeMonitoredItemCreateRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  MonitoredItemCreateRequest request{
      .item_to_monitor = DecodeReadValueId(RequireField(obj, "ItemToMonitor")),
      .monitoring_mode =
          DecodeEnum<MonitoringMode>(RequireField(obj, "MonitoringMode")),
      .requested_parameters =
          DecodeMonitoringParameters(RequireField(obj, "RequestedParameters")),
  };
  if (const auto* field = FindField(obj, "IndexRange"))
    request.index_range = std::string(RequireString(*field));
  return request;
}

value EncodeMonitoredItemCreateResult(
    const MonitoredItemCreateResult& result) {
  object json{{"StatusCode", EncodeStatusCode(result.status.code())},
              {"MonitoredItemId", result.monitored_item_id},
              {"RevisedSamplingInterval", result.revised_sampling_interval_ms},
              {"RevisedQueueSize", result.revised_queue_size}};
  if (result.filter_result.has_value())
    json["FilterResult"] = *result.filter_result;
  return json;
}

MonitoredItemCreateResult DecodeMonitoredItemCreateResult(
    const value& json) {
  const auto& obj = RequireObject(json);
  const auto* status_field = FindField(obj, "StatusCode");
  const auto* legacy_status_field = FindField(obj, "Status");
  if (!status_field && !legacy_status_field)
    ThrowJsonError("Missing required field");
  MonitoredItemCreateResult result{
      .status = status_field ? scada::Status{DecodeStatusCode(*status_field)}
                             : DecodeStatus(*legacy_status_field),
      .monitored_item_id = static_cast<MonitoredItemId>(
          RequireUInt64(RequireField(obj, "MonitoredItemId"))),
      .revised_sampling_interval_ms =
          RequireDouble(RequireField(obj, "RevisedSamplingInterval")),
      .revised_queue_size = static_cast<scada::UInt32>(
          RequireUInt64(RequireField(obj, "RevisedQueueSize"))),
  };
  if (const auto* field = FindField(obj, "FilterResult"))
    result.filter_result = *field;
  return result;
}

value EncodeMonitoredItemModifyRequest(
    const MonitoredItemModifyRequest& request) {
  return object{
      {"MonitoredItemId", request.monitored_item_id},
      {"RequestedParameters",
       EncodeMonitoringParameters(request.requested_parameters)}};
}

MonitoredItemModifyRequest DecodeMonitoredItemModifyRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.monitored_item_id = static_cast<MonitoredItemId>(
              RequireUInt64(RequireField(obj, "MonitoredItemId"))),
          .requested_parameters =
              DecodeMonitoringParameters(RequireField(obj, "RequestedParameters"))};
}

value EncodeMonitoredItemModifyResult(
    const MonitoredItemModifyResult& result) {
  object json{{"StatusCode", EncodeStatusCode(result.status.code())},
              {"RevisedSamplingInterval", result.revised_sampling_interval_ms},
              {"RevisedQueueSize", result.revised_queue_size}};
  if (result.filter_result.has_value())
    json["FilterResult"] = *result.filter_result;
  return json;
}

MonitoredItemModifyResult DecodeMonitoredItemModifyResult(
    const value& json) {
  const auto& obj = RequireObject(json);
  const auto* status_field = FindField(obj, "StatusCode");
  const auto* legacy_status_field = FindField(obj, "Status");
  if (!status_field && !legacy_status_field)
    ThrowJsonError("Missing required field");
  MonitoredItemModifyResult result{
      .status = status_field ? scada::Status{DecodeStatusCode(*status_field)}
                             : DecodeStatus(*legacy_status_field),
      .revised_sampling_interval_ms =
          RequireDouble(RequireField(obj, "RevisedSamplingInterval")),
      .revised_queue_size = static_cast<scada::UInt32>(
          RequireUInt64(RequireField(obj, "RevisedQueueSize"))),
  };
  if (const auto* field = FindField(obj, "FilterResult"))
    result.filter_result = *field;
  return result;
}

value EncodeSubscriptionParameters(
    const SubscriptionParameters& parameters) {
  return object{{"PublishingInterval", parameters.publishing_interval_ms},
                {"LifetimeCount", parameters.lifetime_count},
                {"MaxKeepAliveCount", parameters.max_keep_alive_count},
                {"MaxNotificationsPerPublish",
                 parameters.max_notifications_per_publish},
                {"PublishingEnabled", parameters.publishing_enabled},
                {"Priority", parameters.priority}};
}

SubscriptionParameters DecodeSubscriptionParameters(const value& json) {
  const auto& obj = RequireObject(json);
  return {.publishing_interval_ms =
              RequireDouble(RequireField(obj, "PublishingInterval")),
          .lifetime_count = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "LifetimeCount"))),
          .max_keep_alive_count = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "MaxKeepAliveCount"))),
          .max_notifications_per_publish = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "MaxNotificationsPerPublish"))),
          .publishing_enabled =
              RequireBool(RequireField(obj, "PublishingEnabled")),
          .priority =
              static_cast<scada::UInt8>(RequireUInt64(RequireField(obj, "Priority")))};
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
          .results = DecodeList<scada::StatusCode>(
              RequireField(obj, "Results"), DecodeStatusCode)};
}

}  // namespace

value EncodeCreateSubscriptionRequest(
    const CreateSubscriptionRequest& request) {
  return object{{"RequestedParameters",
                 EncodeSubscriptionParameters(request.parameters)}};
}

CreateSubscriptionRequest DecodeCreateSubscriptionRequest(
    const value& json) {
  return {.parameters =
              DecodeSubscriptionParameters(
                  RequireField(RequireObject(json), "RequestedParameters"))};
}

value EncodeCreateSubscriptionResponse(
    const CreateSubscriptionResponse& response) {
  return object{
      {"Status", EncodeStatus(response.status)},
      {"SubscriptionId", response.subscription_id},
      {"RevisedPublishingInterval", response.revised_publishing_interval_ms},
      {"RevisedLifetimeCount", response.revised_lifetime_count},
      {"RevisedMaxKeepAliveCount", response.revised_max_keep_alive_count}};
}

CreateSubscriptionResponse DecodeCreateSubscriptionResponse(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .revised_publishing_interval_ms =
              RequireDouble(RequireField(obj, "RevisedPublishingInterval")),
          .revised_lifetime_count = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "RevisedLifetimeCount"))),
          .revised_max_keep_alive_count = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "RevisedMaxKeepAliveCount")))};
}

value EncodeModifySubscriptionRequest(
    const ModifySubscriptionRequest& request) {
  return object{{"SubscriptionId", request.subscription_id},
                {"RequestedParameters",
                 EncodeSubscriptionParameters(request.parameters)}};
}

ModifySubscriptionRequest DecodeModifySubscriptionRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .parameters =
              DecodeSubscriptionParameters(RequireField(obj, "RequestedParameters"))};
}

value EncodeModifySubscriptionResponse(
    const ModifySubscriptionResponse& response) {
  return object{
      {"Status", EncodeStatus(response.status)},
      {"RevisedPublishingInterval", response.revised_publishing_interval_ms},
      {"RevisedLifetimeCount", response.revised_lifetime_count},
      {"RevisedMaxKeepAliveCount", response.revised_max_keep_alive_count}};
}

ModifySubscriptionResponse DecodeModifySubscriptionResponse(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .revised_publishing_interval_ms =
              RequireDouble(RequireField(obj, "RevisedPublishingInterval")),
          .revised_lifetime_count = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "RevisedLifetimeCount"))),
          .revised_max_keep_alive_count = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "RevisedMaxKeepAliveCount")))};
}

value EncodeSetPublishingModeRequest(
    const SetPublishingModeRequest& request) {
  return object{{"PublishingEnabled", request.publishing_enabled},
                {"SubscriptionIds",
                 EncodeList(request.subscription_ids, [](auto id) {
                   return value(static_cast<std::uint64_t>(id));
                 })}};
}

SetPublishingModeRequest DecodeSetPublishingModeRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.publishing_enabled =
              RequireBool(RequireField(obj, "PublishingEnabled")),
          .subscription_ids = DecodeList<SubscriptionId>(
              RequireField(obj, "SubscriptionIds"), [](const value& entry) {
                return static_cast<SubscriptionId>(RequireUInt64(entry));
              })};
}

value EncodeSetPublishingModeResponse(
    const SetPublishingModeResponse& response) {
  return EncodeMultiStatusResponse(response);
}

SetPublishingModeResponse DecodeSetPublishingModeResponse(
    const value& json) {
  return DecodeMultiStatusResponse<SetPublishingModeResponse>(json);
}

value EncodeDeleteSubscriptionsRequest(
    const DeleteSubscriptionsRequest& request) {
  return object{{"SubscriptionIds",
                 EncodeList(request.subscription_ids, [](auto id) {
                   return value(static_cast<std::uint64_t>(id));
                 })}};
}

DeleteSubscriptionsRequest DecodeDeleteSubscriptionsRequest(
    const value& json) {
  return {.subscription_ids = DecodeList<SubscriptionId>(
              RequireField(RequireObject(json), "SubscriptionIds"),
              [](const value& entry) {
                return static_cast<SubscriptionId>(RequireUInt64(entry));
              })};
}

value EncodeDeleteSubscriptionsResponse(
    const DeleteSubscriptionsResponse& response) {
  return EncodeMultiStatusResponse(response);
}

DeleteSubscriptionsResponse DecodeDeleteSubscriptionsResponse(
    const value& json) {
  return DecodeMultiStatusResponse<DeleteSubscriptionsResponse>(json);
}

value EncodeCreateMonitoredItemsRequest(
    const CreateMonitoredItemsRequest& request) {
  return object{
      {"SubscriptionId", request.subscription_id},
      {"TimestampsToReturn", EncodeEnum(request.timestamps_to_return)},
      {"ItemsToCreate",
       EncodeList(request.items_to_create, EncodeMonitoredItemCreateRequest)}};
}

CreateMonitoredItemsRequest DecodeCreateMonitoredItemsRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .timestamps_to_return = DecodeEnum<TimestampsToReturn>(
              RequireField(obj, "TimestampsToReturn")),
          .items_to_create = DecodeList<MonitoredItemCreateRequest>(
              RequireField(obj, "ItemsToCreate"),
              DecodeMonitoredItemCreateRequest)};
}

value EncodeCreateMonitoredItemsResponse(
    const CreateMonitoredItemsResponse& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"Results",
                 EncodeList(response.results, EncodeMonitoredItemCreateResult)}};
}

CreateMonitoredItemsResponse DecodeCreateMonitoredItemsResponse(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<MonitoredItemCreateResult>(
              RequireField(obj, "Results"), DecodeMonitoredItemCreateResult)};
}

value EncodeModifyMonitoredItemsRequest(
    const ModifyMonitoredItemsRequest& request) {
  return object{
      {"SubscriptionId", request.subscription_id},
      {"TimestampsToReturn", EncodeEnum(request.timestamps_to_return)},
      {"ItemsToModify",
       EncodeList(request.items_to_modify, EncodeMonitoredItemModifyRequest)}};
}

ModifyMonitoredItemsRequest DecodeModifyMonitoredItemsRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .timestamps_to_return = DecodeEnum<TimestampsToReturn>(
              RequireField(obj, "TimestampsToReturn")),
          .items_to_modify = DecodeList<MonitoredItemModifyRequest>(
              RequireField(obj, "ItemsToModify"),
              DecodeMonitoredItemModifyRequest)};
}

value EncodeModifyMonitoredItemsResponse(
    const ModifyMonitoredItemsResponse& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"Results",
                 EncodeList(response.results, EncodeMonitoredItemModifyResult)}};
}

ModifyMonitoredItemsResponse DecodeModifyMonitoredItemsResponse(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .results = DecodeList<MonitoredItemModifyResult>(
              RequireField(obj, "Results"), DecodeMonitoredItemModifyResult)};
}

value EncodeDeleteMonitoredItemsRequest(
    const DeleteMonitoredItemsRequest& request) {
  return object{{"SubscriptionId", request.subscription_id},
                {"MonitoredItemIds",
                 EncodeList(request.monitored_item_ids, [](auto id) {
                   return value(static_cast<std::uint64_t>(id));
                 })}};
}

DeleteMonitoredItemsRequest DecodeDeleteMonitoredItemsRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .monitored_item_ids = DecodeList<MonitoredItemId>(
              RequireField(obj, "MonitoredItemIds"), [](const value& entry) {
                return static_cast<MonitoredItemId>(RequireUInt64(entry));
              })};
}

value EncodeDeleteMonitoredItemsResponse(
    const DeleteMonitoredItemsResponse& response) {
  return EncodeMultiStatusResponse(response);
}

DeleteMonitoredItemsResponse DecodeDeleteMonitoredItemsResponse(
    const value& json) {
  return DecodeMultiStatusResponse<DeleteMonitoredItemsResponse>(json);
}

value EncodeSetMonitoringModeRequest(
    const SetMonitoringModeRequest& request) {
  return object{{"SubscriptionId", request.subscription_id},
                {"MonitoringMode", EncodeEnum(request.monitoring_mode)},
                {"MonitoredItemIds",
                 EncodeList(request.monitored_item_ids, [](auto id) {
                   return value(static_cast<std::uint64_t>(id));
                 })}};
}

SetMonitoringModeRequest DecodeSetMonitoringModeRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .monitoring_mode = DecodeEnum<MonitoringMode>(
              RequireField(obj, "MonitoringMode")),
          .monitored_item_ids = DecodeList<MonitoredItemId>(
              RequireField(obj, "MonitoredItemIds"), [](const value& entry) {
                return static_cast<MonitoredItemId>(RequireUInt64(entry));
              })};
}

value EncodeSetMonitoringModeResponse(
    const SetMonitoringModeResponse& response) {
  return EncodeMultiStatusResponse(response);
}

SetMonitoringModeResponse DecodeSetMonitoringModeResponse(
    const value& json) {
  return DecodeMultiStatusResponse<SetMonitoringModeResponse>(json);
}

}  // namespace opcua::ws::detail
