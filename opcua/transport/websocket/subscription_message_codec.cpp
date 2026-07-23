#include "opcua/transport/websocket/json_codec.h"

#include "opcua/session/subscription_conversion.h"
#include "opcua/ua/ua_json_codec.h"

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

std::uint64_t RequireUInt64(const value& json) {
  if (json.is_uint64())
    return json.as_uint64();
  if (json.is_int64() && json.as_int64() >= 0)
    return static_cast<std::uint64_t>(json.as_int64());
  ThrowJsonError("Expected JSON unsigned integer");
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

// CreateSubscription / ModifySubscription use the generated, spec-conformant
// OPC UA JSON codec (Part 6 §5.4), bridged to the hand-written subscription
// vocabulary by subscription_conversion.

value EncodeCreateSubscriptionRequest(
    const CreateSubscriptionRequest& request) {
  return ua::EncodeJson(subscription_conversion::ToWire(request));
}

CreateSubscriptionRequest DecodeCreateSubscriptionRequest(const value& json) {
  ua::CreateSubscriptionRequest wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeCreateSubscriptionResponse(
    const CreateSubscriptionResponse& response) {
  return ua::EncodeJson(subscription_conversion::ToWire(response));
}

CreateSubscriptionResponse DecodeCreateSubscriptionResponse(const value& json) {
  ua::CreateSubscriptionResponse wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeModifySubscriptionRequest(
    const ModifySubscriptionRequest& request) {
  return ua::EncodeJson(subscription_conversion::ToWire(request));
}

ModifySubscriptionRequest DecodeModifySubscriptionRequest(const value& json) {
  ua::ModifySubscriptionRequest wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeModifySubscriptionResponse(
    const ModifySubscriptionResponse& response) {
  return ua::EncodeJson(subscription_conversion::ToWire(response));
}

ModifySubscriptionResponse DecodeModifySubscriptionResponse(const value& json) {
  ua::ModifySubscriptionResponse wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

// Migrated to the generated conformant OPC UA JSON codec (Part 6 §5.4).
value EncodeSetPublishingModeRequest(
    const ua::SetPublishingModeRequest& request) {
  return ua::EncodeJson(request);
}

ua::SetPublishingModeRequest DecodeSetPublishingModeRequest(const value& json) {
  ua::SetPublishingModeRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeSetPublishingModeResponse(
    const ua::SetPublishingModeResponse& response) {
  return ua::EncodeJson(response);
}

ua::SetPublishingModeResponse DecodeSetPublishingModeResponse(
    const value& json) {
  ua::SetPublishingModeResponse response;
  ua::DecodeJson(json, response);
  return response;
}

// Migrated to the generated conformant OPC UA JSON codec (Part 6 §5.4). The
// body is `{RequestHeader, SubscriptionIds}` / `{ResponseHeader, Results,
// DiagnosticInfos}` — the same shape the web client's generated codec produces,
// replacing the earlier flat `{SubscriptionIds}` / `{Status, Results}` form.
value EncodeDeleteSubscriptionsRequest(
    const ua::DeleteSubscriptionsRequest& request) {
  return ua::EncodeJson(request);
}

ua::DeleteSubscriptionsRequest DecodeDeleteSubscriptionsRequest(
    const value& json) {
  ua::DeleteSubscriptionsRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeDeleteSubscriptionsResponse(
    const ua::DeleteSubscriptionsResponse& response) {
  return ua::EncodeJson(response);
}

ua::DeleteSubscriptionsResponse DecodeDeleteSubscriptionsResponse(
    const value& json) {
  ua::DeleteSubscriptionsResponse response;
  ua::DecodeJson(json, response);
  return response;
}

// CreateMonitoredItems / ModifyMonitoredItems use the generated,
// spec-conformant OPC UA JSON codec (Part 6 §5.4). The monitored-item filter
// (DataChangeFilter / EventFilter) now travels as a conformant ExtensionObject,
// bridged to the hand-written MonitoringFilter by subscription_conversion.

value EncodeCreateMonitoredItemsRequest(
    const CreateMonitoredItemsRequest& request) {
  return ua::EncodeJson(subscription_conversion::ToWire(request));
}

CreateMonitoredItemsRequest DecodeCreateMonitoredItemsRequest(
    const value& json) {
  ua::CreateMonitoredItemsRequest wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeCreateMonitoredItemsResponse(
    const CreateMonitoredItemsResponse& response) {
  return ua::EncodeJson(subscription_conversion::ToWire(response));
}

CreateMonitoredItemsResponse DecodeCreateMonitoredItemsResponse(
    const value& json) {
  ua::CreateMonitoredItemsResponse wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeModifyMonitoredItemsRequest(
    const ModifyMonitoredItemsRequest& request) {
  return ua::EncodeJson(subscription_conversion::ToWire(request));
}

ModifyMonitoredItemsRequest DecodeModifyMonitoredItemsRequest(
    const value& json) {
  ua::ModifyMonitoredItemsRequest wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeModifyMonitoredItemsResponse(
    const ModifyMonitoredItemsResponse& response) {
  return ua::EncodeJson(subscription_conversion::ToWire(response));
}

ModifyMonitoredItemsResponse DecodeModifyMonitoredItemsResponse(
    const value& json) {
  ua::ModifyMonitoredItemsResponse wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

// Migrated to the generated conformant OPC UA JSON codec (Part 6 §5.4).
value EncodeDeleteMonitoredItemsRequest(
    const ua::DeleteMonitoredItemsRequest& request) {
  return ua::EncodeJson(request);
}

ua::DeleteMonitoredItemsRequest DecodeDeleteMonitoredItemsRequest(
    const value& json) {
  ua::DeleteMonitoredItemsRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeDeleteMonitoredItemsResponse(
    const ua::DeleteMonitoredItemsResponse& response) {
  return ua::EncodeJson(response);
}

ua::DeleteMonitoredItemsResponse DecodeDeleteMonitoredItemsResponse(
    const value& json) {
  ua::DeleteMonitoredItemsResponse response;
  ua::DecodeJson(json, response);
  return response;
}

// Migrated to the generated conformant OPC UA JSON codec (Part 6 §5.4).
value EncodeSetMonitoringModeRequest(
    const ua::SetMonitoringModeRequest& request) {
  return ua::EncodeJson(request);
}

ua::SetMonitoringModeRequest DecodeSetMonitoringModeRequest(const value& json) {
  ua::SetMonitoringModeRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeSetMonitoringModeResponse(
    const ua::SetMonitoringModeResponse& response) {
  return ua::EncodeJson(response);
}

ua::SetMonitoringModeResponse DecodeSetMonitoringModeResponse(
    const value& json) {
  ua::SetMonitoringModeResponse response;
  ua::DecodeJson(json, response);
  return response;
}

}  // namespace opcua::ws::detail
