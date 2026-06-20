#include "opcua/websocket/json_codec.h"

#include "opcua/base/time_utils.h"

#include <boost/json.hpp>

#include <format>
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

value EncodeDateTime(scada::DateTime time) {
  if (time.is_null())
    return string("0001-01-01T00:00:00Z");
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
  scada::DateTime result;
  if (!Deserialize(RequireString(json), result))
    ThrowJsonError("Invalid DateTime");
  return result;
}

value EncodeVariant(const scada::Variant& variant) {
  CallRequest request{.methods = {{.object_id = scada::NodeId{},
                                   .method_id = scada::NodeId{},
                                   .arguments = {variant}}}};
  const auto service_json = RequireObject(EncodeJson(ServiceRequest{request}));
  const auto& body = RequireObject(RequireField(service_json, "body"));
  const auto& methods = RequireArray(RequireField(body, "MethodsToCall"));
  return RequireArray(
             RequireField(RequireObject(methods.front()), "InputArguments"))
      .front();
}

scada::Variant DecodeVariant(const value& json) {
  object method;
  method["ObjectId"] = "i=0";
  method["MethodId"] = "i=0";
  method["InputArguments"] = array{json};
  object body;
  body["MethodsToCall"] = array{std::move(method)};
  const object wrapper{{"service", "Call"}, {"body", std::move(body)}};
  const auto request = std::get<CallRequest>(*DecodeServiceRequest(wrapper));
  return request.methods.front().arguments.front();
}

value EncodeDataValue(const scada::DataValue& data_value) {
  ReadResponse response{.status = scada::StatusCode::Good,
                        .results = {data_value}};
  const auto service_json =
      RequireObject(EncodeJson(ServiceResponse{response}));
  const auto& body = RequireObject(RequireField(service_json, "body"));
  return RequireArray(RequireField(body, "Results")).front();
}

scada::DataValue DecodeDataValue(const value& json) {
  const object wrapper{
      {"service", "Read"},
      {"body", object{{"Status", 0u}, {"Results", array{json}}}}};
  const auto response = std::get<ReadResponse>(*DecodeServiceResponse(wrapper));
  return response.results.front();
}

value EncodeSubscriptionAcknowledgement(
    const SubscriptionAcknowledgement& acknowledgement) {
  return object{{"SubscriptionId", acknowledgement.subscription_id},
                {"SequenceNumber", acknowledgement.sequence_number}};
}

SubscriptionAcknowledgement DecodeSubscriptionAcknowledgement(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .sequence_number = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "SequenceNumber")))};
}

value EncodeMonitoredItemNotification(
    const MonitoredItemNotification& notification) {
  return object{{"ClientHandle", notification.client_handle},
                {"Value", EncodeDataValue(notification.value)}};
}

MonitoredItemNotification DecodeMonitoredItemNotification(const value& json) {
  const auto& obj = RequireObject(json);
  return {.client_handle = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "ClientHandle"))),
          .value = DecodeDataValue(RequireField(obj, "Value"))};
}

value EncodeEventFieldList(const EventFieldList& fields) {
  return object{
      {"ClientHandle", fields.client_handle},
      {"EventFields", EncodeList(fields.event_fields, EncodeVariant)}};
}

EventFieldList DecodeEventFieldList(const value& json) {
  const auto& obj = RequireObject(json);
  return {.client_handle = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "ClientHandle"))),
          .event_fields = DecodeList<scada::Variant>(
              RequireField(obj, "EventFields"), DecodeVariant)};
}

value EncodeNotificationData(const NotificationData& notification) {
  return std::visit(
      [](const auto& typed_notification) -> value {
        using T = std::decay_t<decltype(typed_notification)>;
        if constexpr (std::is_same_v<T, DataChangeNotification>) {
          return object{
              {"Type", "DataChangeNotification"},
              {"MonitoredItems", EncodeList(typed_notification.monitored_items,
                                            EncodeMonitoredItemNotification)}};
        } else if constexpr (std::is_same_v<T, EventNotificationList>) {
          return object{{"Type", "EventNotificationList"},
                        {"Events", EncodeList(typed_notification.events,
                                              EncodeEventFieldList)}};
        } else {
          return object{
              {"Type", "StatusChangeNotification"},
              {"Status", EncodeStatusCode(typed_notification.status)}};
        }
      },
      notification);
}

NotificationData DecodeNotificationData(const value& json) {
  const auto& obj = RequireObject(json);
  const auto type = RequireString(RequireField(obj, "Type"));
  if (type == "DataChangeNotification") {
    return DataChangeNotification{.monitored_items =
                                      DecodeList<MonitoredItemNotification>(
                                          RequireField(obj, "MonitoredItems"),
                                          DecodeMonitoredItemNotification)};
  }
  if (type == "EventNotificationList") {
    return EventNotificationList{
        .events = DecodeList<EventFieldList>(RequireField(obj, "Events"),
                                             DecodeEventFieldList)};
  }
  if (type == "StatusChangeNotification") {
    return StatusChangeNotification{
        .status = DecodeStatusCode(RequireField(obj, "Status"))};
  }
  ThrowJsonError("Unknown NotificationData type");
}

value EncodeNotificationMessage(
    const NotificationMessage& notification_message) {
  return object{
      {"SequenceNumber", notification_message.sequence_number},
      {"PublishTime", EncodeDateTime(notification_message.publish_time)},
      {"NotificationData", EncodeList(notification_message.notification_data,
                                      EncodeNotificationData)}};
}

NotificationMessage DecodeNotificationMessage(const value& json) {
  const auto& obj = RequireObject(json);
  return {.sequence_number = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "SequenceNumber"))),
          .publish_time = DecodeDateTime(RequireField(obj, "PublishTime")),
          .notification_data = DecodeList<NotificationData>(
              RequireField(obj, "NotificationData"), DecodeNotificationData)};
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

}  // namespace

value EncodePublishRequest(const PublishRequest& request) {
  return object{{"SubscriptionAcknowledgements",
                 EncodeList(request.subscription_acknowledgements,
                            EncodeSubscriptionAcknowledgement)}};
}

PublishRequest DecodePublishRequest(const value& json) {
  return {
      .subscription_acknowledgements = DecodeList<SubscriptionAcknowledgement>(
          RequireField(RequireObject(json), "SubscriptionAcknowledgements"),
          DecodeSubscriptionAcknowledgement)};
}

value EncodePublishResponse(const PublishResponse& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"SubscriptionId", response.subscription_id},
                {"AvailableSequenceNumbers",
                 EncodeList(response.available_sequence_numbers,
                            [](auto value) {
                              return boost::json::value(
                                  static_cast<std::uint64_t>(value));
                            })},
                {"MoreNotifications", response.more_notifications},
                {"NotificationMessage",
                 EncodeNotificationMessage(response.notification_message)},
                {"Results", EncodeList(response.results, EncodeStatusCode)}};
}

PublishResponse DecodePublishResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .status = DecodeStatus(RequireField(obj, "Status")),
      .subscription_id = static_cast<SubscriptionId>(
          RequireUInt64(RequireField(obj, "SubscriptionId"))),
      .results = DecodeList<scada::StatusCode>(RequireField(obj, "Results"),
                                               DecodeStatusCode),
      .more_notifications = RequireBool(RequireField(obj, "MoreNotifications")),
      .notification_message =
          DecodeNotificationMessage(RequireField(obj, "NotificationMessage")),
      .available_sequence_numbers = DecodeList<scada::UInt32>(
          RequireField(obj, "AvailableSequenceNumbers"),
          [](const value& entry) {
            return static_cast<scada::UInt32>(RequireUInt64(entry));
          })};
}

value EncodeRepublishRequest(const RepublishRequest& request) {
  return object{
      {"SubscriptionId", request.subscription_id},
      {"RetransmitSequenceNumber", request.retransmit_sequence_number}};
}

RepublishRequest DecodeRepublishRequest(const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_id = static_cast<SubscriptionId>(
              RequireUInt64(RequireField(obj, "SubscriptionId"))),
          .retransmit_sequence_number = static_cast<scada::UInt32>(
              RequireUInt64(RequireField(obj, "RetransmitSequenceNumber")))};
}

value EncodeRepublishResponse(const RepublishResponse& response) {
  return object{{"Status", EncodeStatus(response.status)},
                {"NotificationMessage",
                 EncodeNotificationMessage(response.notification_message)}};
}

RepublishResponse DecodeRepublishResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .notification_message = DecodeNotificationMessage(
              RequireField(obj, "NotificationMessage"))};
}

value EncodeTransferSubscriptionsRequest(
    const TransferSubscriptionsRequest& request) {
  return object{
      {"SubscriptionIds", EncodeList(request.subscription_ids,
                                     [](auto value) {
                                       return boost::json::value(
                                           static_cast<std::uint64_t>(value));
                                     })},
      {"SendInitialValues", request.send_initial_values}};
}

TransferSubscriptionsRequest DecodeTransferSubscriptionsRequest(
    const value& json) {
  const auto& obj = RequireObject(json);
  return {.subscription_ids = DecodeList<SubscriptionId>(
              RequireField(obj, "SubscriptionIds"),
              [](const value& entry) {
                return static_cast<SubscriptionId>(RequireUInt64(entry));
              }),
          .send_initial_values =
              RequireBool(RequireField(obj, "SendInitialValues"))};
}

value EncodeTransferSubscriptionsResponse(
    const TransferSubscriptionsResponse& response) {
  return EncodeMultiStatusResponse(response);
}

TransferSubscriptionsResponse DecodeTransferSubscriptionsResponse(
    const value& json) {
  return DecodeMultiStatusResponse<TransferSubscriptionsResponse>(json);
}

}  // namespace opcua::ws::detail
