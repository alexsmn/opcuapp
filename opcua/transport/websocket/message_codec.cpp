#include "opcua/transport/websocket/json_codec.h"

#include "opcua/base/utf_convert.h"
#include "opcua/session/discovery_conversion.h"
#include "opcua/session/session_conversion.h"
#include "opcua/ua/ua_json_codec.h"

#include <boost/json.hpp>

#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace opcua::ws {

namespace detail {
boost::json::value EncodeCreateSubscriptionRequest(
    const CreateSubscriptionRequest& request);
CreateSubscriptionRequest DecodeCreateSubscriptionRequest(
    const boost::json::value& json);
boost::json::value EncodeCreateSubscriptionResponse(
    const CreateSubscriptionResponse& response);
CreateSubscriptionResponse DecodeCreateSubscriptionResponse(
    const boost::json::value& json);
boost::json::value EncodeModifySubscriptionRequest(
    const ModifySubscriptionRequest& request);
ModifySubscriptionRequest DecodeModifySubscriptionRequest(
    const boost::json::value& json);
boost::json::value EncodeModifySubscriptionResponse(
    const ModifySubscriptionResponse& response);
ModifySubscriptionResponse DecodeModifySubscriptionResponse(
    const boost::json::value& json);
boost::json::value EncodeSetPublishingModeRequest(
    const ua::SetPublishingModeRequest& request);
ua::SetPublishingModeRequest DecodeSetPublishingModeRequest(
    const boost::json::value& json);
boost::json::value EncodeSetPublishingModeResponse(
    const ua::SetPublishingModeResponse& response);
ua::SetPublishingModeResponse DecodeSetPublishingModeResponse(
    const boost::json::value& json);
boost::json::value EncodeDeleteSubscriptionsRequest(
    const ua::DeleteSubscriptionsRequest& request);
ua::DeleteSubscriptionsRequest DecodeDeleteSubscriptionsRequest(
    const boost::json::value& json);
boost::json::value EncodeDeleteSubscriptionsResponse(
    const ua::DeleteSubscriptionsResponse& response);
ua::DeleteSubscriptionsResponse DecodeDeleteSubscriptionsResponse(
    const boost::json::value& json);
boost::json::value EncodePublishRequest(const PublishRequest& request);
PublishRequest DecodePublishRequest(const boost::json::value& json);
boost::json::value EncodePublishResponse(const PublishResponse& response);
PublishResponse DecodePublishResponse(const boost::json::value& json);
boost::json::value EncodeRepublishRequest(const RepublishRequest& request);
RepublishRequest DecodeRepublishRequest(const boost::json::value& json);
boost::json::value EncodeRepublishResponse(const RepublishResponse& response);
RepublishResponse DecodeRepublishResponse(const boost::json::value& json);
boost::json::value EncodeTransferSubscriptionsRequest(
    const ua::TransferSubscriptionsRequest& request);
ua::TransferSubscriptionsRequest DecodeTransferSubscriptionsRequest(
    const boost::json::value& json);
boost::json::value EncodeTransferSubscriptionsResponse(
    const ua::TransferSubscriptionsResponse& response);
ua::TransferSubscriptionsResponse DecodeTransferSubscriptionsResponse(
    const boost::json::value& json);
boost::json::value EncodeCreateMonitoredItemsRequest(
    const CreateMonitoredItemsRequest& request);
CreateMonitoredItemsRequest DecodeCreateMonitoredItemsRequest(
    const boost::json::value& json);
boost::json::value EncodeCreateMonitoredItemsResponse(
    const CreateMonitoredItemsResponse& response);
CreateMonitoredItemsResponse DecodeCreateMonitoredItemsResponse(
    const boost::json::value& json);
boost::json::value EncodeModifyMonitoredItemsRequest(
    const ModifyMonitoredItemsRequest& request);
ModifyMonitoredItemsRequest DecodeModifyMonitoredItemsRequest(
    const boost::json::value& json);
boost::json::value EncodeModifyMonitoredItemsResponse(
    const ModifyMonitoredItemsResponse& response);
ModifyMonitoredItemsResponse DecodeModifyMonitoredItemsResponse(
    const boost::json::value& json);
boost::json::value EncodeDeleteMonitoredItemsRequest(
    const ua::DeleteMonitoredItemsRequest& request);
ua::DeleteMonitoredItemsRequest DecodeDeleteMonitoredItemsRequest(
    const boost::json::value& json);
boost::json::value EncodeDeleteMonitoredItemsResponse(
    const ua::DeleteMonitoredItemsResponse& response);
ua::DeleteMonitoredItemsResponse DecodeDeleteMonitoredItemsResponse(
    const boost::json::value& json);
boost::json::value EncodeSetMonitoringModeRequest(
    const ua::SetMonitoringModeRequest& request);
ua::SetMonitoringModeRequest DecodeSetMonitoringModeRequest(
    const boost::json::value& json);
boost::json::value EncodeSetMonitoringModeResponse(
    const ua::SetMonitoringModeResponse& response);
ua::SetMonitoringModeResponse DecodeSetMonitoringModeResponse(
    const boost::json::value& json);
}  // namespace detail

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

double RequireDouble(const value& json) {
  if (json.is_double())
    return json.as_double();
  if (json.is_int64())
    return static_cast<double>(json.as_int64());
  if (json.is_uint64())
    return static_cast<double>(json.as_uint64());
  ThrowJsonError("Expected JSON number");
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

// OPC UA Part 6 §5.4.2.14: LocalizedText is an object `{ Locale?, Text? }`,
// each field omitted when null/empty,
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
    const auto raw = RequireUInt64(entry);
    if (raw > std::numeric_limits<unsigned char>::max())
      ThrowJsonError("ByteString element out of range");
    bytes.push_back(static_cast<char>(static_cast<unsigned char>(raw)));
  }
  return bytes;
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

value EncodeAttributeId(AttributeId attribute_id) {
  return static_cast<std::uint64_t>(static_cast<unsigned>(attribute_id));
}

AttributeId DecodeAttributeId(const value& json) {
  return static_cast<AttributeId>(static_cast<unsigned>(RequireUInt64(json)));
}

template <class T, class Encoder>
array EncodeList(const std::vector<T>& values, Encoder&& encoder) {
  array result;
  result.reserve(values.size());
  for (const auto& value : values)
    result.emplace_back(encoder(value));
  return result;
}

template <class T, class Decoder>
std::vector<T> DecodeList(const value& json, Decoder&& decoder) {
  std::vector<T> result;
  for (const auto& entry : RequireArray(json))
    result.emplace_back(decoder(entry));
  return result;
}

// The session services use the generated, spec-conformant OPC UA JSON codec
// (OPC UA Part 6 §5.4): the request/response bodies are the schema-derived
// shapes, and the session vocabulary the runtime and manager use is bridged by
// session_conversion. The authentication token lives in the body's
// RequestHeader (the conformant binding); the top-level envelope still carries
// `requestHandle` for correlation.

value EncodeCreateSessionRequest(const CreateSessionRequest& request) {
  return ua::EncodeJson(session_conversion::ToWire(request));
}

CreateSessionRequest DecodeCreateSessionRequest(const value& json) {
  ua::CreateSessionRequest wire;
  ua::DecodeJson(json, wire);
  return session_conversion::ToManaged(wire);
}

value EncodeCreateSessionResponse(const CreateSessionResponse& response) {
  return ua::EncodeJson(session_conversion::ToWire(response));
}

CreateSessionResponse DecodeCreateSessionResponse(const value& json) {
  ua::CreateSessionResponse wire;
  ua::DecodeJson(json, wire);
  return session_conversion::ToManaged(wire);
}

value EncodeActivateSessionRequest(const ActivateSessionRequest& request) {
  ua::ActivateSessionRequest wire = session_conversion::ToWire(request);
  // The session is identified by the authentication token in the header.
  wire.request_header.authentication_token = request.authentication_token;
  return ua::EncodeJson(wire);
}

ActivateSessionRequest DecodeActivateSessionRequest(const value& json) {
  ua::ActivateSessionRequest wire;
  ua::DecodeJson(json, wire);
  auto request = session_conversion::ToManaged(wire);
  if (!request)
    ThrowJsonError("Invalid ActivateSession user identity token");
  return std::move(*request);
}

value EncodeActivateSessionResponse(const ActivateSessionResponse& response) {
  return ua::EncodeJson(session_conversion::ToWire(response));
}

ActivateSessionResponse DecodeActivateSessionResponse(const value& json) {
  ua::ActivateSessionResponse wire;
  ua::DecodeJson(json, wire);
  return session_conversion::ToManaged(wire);
}

value EncodeCloseSessionRequest(const CloseSessionRequest& request) {
  ua::CloseSessionRequest wire = session_conversion::ToWire(request);
  wire.request_header.authentication_token = request.authentication_token;
  return ua::EncodeJson(wire);
}

CloseSessionRequest DecodeCloseSessionRequest(const value& json) {
  ua::CloseSessionRequest wire;
  ua::DecodeJson(json, wire);
  return session_conversion::ToManaged(wire);
}

value EncodeCloseSessionResponse(const CloseSessionResponse& response) {
  return ua::EncodeJson(session_conversion::ToWire(response));
}

CloseSessionResponse DecodeCloseSessionResponse(const value& json) {
  ua::CloseSessionResponse wire;
  ua::DecodeJson(json, wire);
  return session_conversion::ToManaged(wire);
}

value EncodeServiceFault(const ServiceFault& fault) {
  return object{{"Status", EncodeStatus(fault.status)}};
}

ServiceFault DecodeServiceFault(const value& json) {
  return {.status = DecodeStatus(RequireField(RequireObject(json), "Status"))};
}

value EncodeStringArray(const std::vector<std::string>& values) {
  array json;
  for (const auto& value : values)
    json.emplace_back(value);
  return json;
}

std::vector<std::string> DecodeStringArray(const value& json) {
  std::vector<std::string> values;
  for (const auto& entry : json.as_array())
    values.emplace_back(RequireString(entry));
  return values;
}

// FindServers / GetEndpoints use the generated, spec-conformant OPC UA JSON
// codec (Part 6 §5.4), bridged to the hand-written discovery vocabulary by
// discovery_conversion. RegisterServer / RegisterServer2 are exposed only over
// UA-TCP binary, so they have no websocket codec.

value EncodeFindServersRequest(const FindServersRequest& request) {
  return ua::EncodeJson(discovery_conversion::ToWire(request));
}

FindServersRequest DecodeFindServersRequest(const value& json) {
  ua::FindServersRequest wire;
  ua::DecodeJson(json, wire);
  return discovery_conversion::ToManaged(wire);
}

value EncodeGetEndpointsRequest(const GetEndpointsRequest& request) {
  return ua::EncodeJson(discovery_conversion::ToWire(request));
}

GetEndpointsRequest DecodeGetEndpointsRequest(const value& json) {
  ua::GetEndpointsRequest wire;
  ua::DecodeJson(json, wire);
  return discovery_conversion::ToManaged(wire);
}

value EncodeFindServersResponse(const FindServersResponse& response) {
  return ua::EncodeJson(discovery_conversion::ToWire(response));
}

FindServersResponse DecodeFindServersResponse(const value& json) {
  ua::FindServersResponse wire;
  ua::DecodeJson(json, wire);
  return discovery_conversion::ToManaged(wire);
}

value EncodeGetEndpointsResponse(const GetEndpointsResponse& response) {
  return ua::EncodeJson(discovery_conversion::ToWire(response));
}

GetEndpointsResponse DecodeGetEndpointsResponse(const value& json) {
  ua::GetEndpointsResponse wire;
  ua::DecodeJson(json, wire);
  return discovery_conversion::ToManaged(wire);
}

}  // namespace

boost::json::value EncodeJson(const RequestMessage& request) {
  return std::visit(
      [&](const auto& typed_request) -> value {
        object json;
        json["requestHandle"] = request.request_handle;
        using T = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<T, FindServersRequest>) {
          json["service"] = "FindServers";
          json["body"] = EncodeFindServersRequest(typed_request);
        } else if constexpr (std::is_same_v<T, GetEndpointsRequest>) {
          json["service"] = "GetEndpoints";
          json["body"] = EncodeGetEndpointsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, CreateSessionRequest>) {
          json["service"] = "CreateSession";
          json["body"] = EncodeCreateSessionRequest(typed_request);
        } else if constexpr (std::is_same_v<T, ActivateSessionRequest>) {
          json["service"] = "ActivateSession";
          json["body"] = EncodeActivateSessionRequest(typed_request);
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
          json["service"] = "CloseSession";
          json["body"] = EncodeCloseSessionRequest(typed_request);
        } else if constexpr (std::is_same_v<T, CreateSubscriptionRequest>) {
          json["service"] = "CreateSubscription";
          json["body"] = detail::EncodeCreateSubscriptionRequest(typed_request);
        } else if constexpr (std::is_same_v<T, ModifySubscriptionRequest>) {
          json["service"] = "ModifySubscription";
          json["body"] = detail::EncodeModifySubscriptionRequest(typed_request);
        } else if constexpr (std::is_same_v<T, ua::SetPublishingModeRequest>) {
          json["service"] = "SetPublishingMode";
          json["body"] = detail::EncodeSetPublishingModeRequest(typed_request);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteSubscriptionsRequest>) {
          json["service"] = "DeleteSubscriptions";
          json["body"] =
              detail::EncodeDeleteSubscriptionsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, PublishRequest>) {
          json["service"] = "Publish";
          json["body"] = detail::EncodePublishRequest(typed_request);
        } else if constexpr (std::is_same_v<T, RepublishRequest>) {
          json["service"] = "Republish";
          json["body"] = detail::EncodeRepublishRequest(typed_request);
        } else if constexpr (std::is_same_v<T,
                                            ua::TransferSubscriptionsRequest>) {
          json["service"] = "TransferSubscriptions";
          json["body"] =
              detail::EncodeTransferSubscriptionsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsRequest>) {
          json["service"] = "CreateMonitoredItems";
          json["body"] =
              detail::EncodeCreateMonitoredItemsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsRequest>) {
          json["service"] = "ModifyMonitoredItems";
          json["body"] =
              detail::EncodeModifyMonitoredItemsRequest(typed_request);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteMonitoredItemsRequest>) {
          json["service"] = "DeleteMonitoredItems";
          json["body"] =
              detail::EncodeDeleteMonitoredItemsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, ua::SetMonitoringModeRequest>) {
          json["service"] = "SetMonitoringMode";
          json["body"] = detail::EncodeSetMonitoringModeRequest(typed_request);
        } else if constexpr (requires { ServiceRequest{typed_request}; }) {
          const auto service_value = EncodeJson(ServiceRequest{typed_request});
          const auto& service_json = RequireObject(service_value);
          json["service"] = service_json.at("service");
          json["body"] = service_json.at("body");
        } else {
          // RegisterNodes / UnregisterNodes are exposed only over UA-TCP
          // binary, not over the WS JSON transport.
          json["service"] = "Unsupported";
        }
        return json;
      },
      request.body);
}

boost::json::value EncodeJson(const ResponseMessage& response) {
  return std::visit(
      [&](const auto& typed_response) -> value {
        object json;
        json["requestHandle"] = response.request_handle;
        using T = std::decay_t<decltype(typed_response)>;
        if constexpr (std::is_same_v<T, FindServersResponse>) {
          json["service"] = "FindServers";
          json["body"] = EncodeFindServersResponse(typed_response);
        } else if constexpr (std::is_same_v<T, GetEndpointsResponse>) {
          json["service"] = "GetEndpoints";
          json["body"] = EncodeGetEndpointsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, CreateSessionResponse>) {
          json["service"] = "CreateSession";
          json["body"] = EncodeCreateSessionResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ActivateSessionResponse>) {
          json["service"] = "ActivateSession";
          json["body"] = EncodeActivateSessionResponse(typed_response);
        } else if constexpr (std::is_same_v<T, CloseSessionResponse>) {
          json["service"] = "CloseSession";
          json["body"] = EncodeCloseSessionResponse(typed_response);
        } else if constexpr (std::is_same_v<T, CreateSubscriptionResponse>) {
          json["service"] = "CreateSubscription";
          json["body"] =
              detail::EncodeCreateSubscriptionResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ModifySubscriptionResponse>) {
          json["service"] = "ModifySubscription";
          json["body"] =
              detail::EncodeModifySubscriptionResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ua::SetPublishingModeResponse>) {
          json["service"] = "SetPublishingMode";
          json["body"] =
              detail::EncodeSetPublishingModeResponse(typed_response);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteSubscriptionsResponse>) {
          json["service"] = "DeleteSubscriptions";
          json["body"] =
              detail::EncodeDeleteSubscriptionsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, PublishResponse>) {
          json["service"] = "Publish";
          json["body"] = detail::EncodePublishResponse(typed_response);
        } else if constexpr (std::is_same_v<T, RepublishResponse>) {
          json["service"] = "Republish";
          json["body"] = detail::EncodeRepublishResponse(typed_response);
        } else if constexpr (std::is_same_v<
                                 T, ua::TransferSubscriptionsResponse>) {
          json["service"] = "TransferSubscriptions";
          json["body"] =
              detail::EncodeTransferSubscriptionsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsResponse>) {
          json["service"] = "CreateMonitoredItems";
          json["body"] =
              detail::EncodeCreateMonitoredItemsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsResponse>) {
          json["service"] = "ModifyMonitoredItems";
          json["body"] =
              detail::EncodeModifyMonitoredItemsResponse(typed_response);
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteMonitoredItemsResponse>) {
          json["service"] = "DeleteMonitoredItems";
          json["body"] =
              detail::EncodeDeleteMonitoredItemsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ua::SetMonitoringModeResponse>) {
          json["service"] = "SetMonitoringMode";
          json["body"] =
              detail::EncodeSetMonitoringModeResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ServiceFault>) {
          json["service"] = "ServiceFault";
          json["body"] = EncodeServiceFault(typed_response);
        } else if constexpr (requires { ServiceResponse{typed_response}; }) {
          const auto service_value =
              EncodeJson(ServiceResponse{typed_response});
          const auto& service_json = RequireObject(service_value);
          json["service"] = service_json.at("service");
          json["body"] = service_json.at("body");
        } else {
          // RegisterNodes / UnregisterNodes are exposed only over UA-TCP
          // binary, not over the WS JSON transport.
          json["service"] = "Unsupported";
        }
        return json;
      },
      response.body);
}

StatusOr<RequestMessage> DecodeRequestMessage(const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    const auto service = RequireString(RequireField(obj, "service"));
    RequestMessage message{
        .request_handle = static_cast<UInt32>(
            RequireUInt64(RequireField(obj, "requestHandle"))),
        .body = CloseSessionRequest{},
    };
    if (service == "FindServers") {
      message.body = DecodeFindServersRequest(body);
    } else if (service == "GetEndpoints") {
      message.body = DecodeGetEndpointsRequest(body);
    } else if (service == "CreateSession") {
      message.body = DecodeCreateSessionRequest(body);
    } else if (service == "ActivateSession") {
      message.body = DecodeActivateSessionRequest(body);
    } else if (service == "CloseSession") {
      message.body = DecodeCloseSessionRequest(body);
    } else if (service == "CreateSubscription") {
      message.body = detail::DecodeCreateSubscriptionRequest(body);
    } else if (service == "ModifySubscription") {
      message.body = detail::DecodeModifySubscriptionRequest(body);
    } else if (service == "SetPublishingMode") {
      message.body = detail::DecodeSetPublishingModeRequest(body);
    } else if (service == "DeleteSubscriptions") {
      message.body = detail::DecodeDeleteSubscriptionsRequest(body);
    } else if (service == "Publish") {
      message.body = detail::DecodePublishRequest(body);
    } else if (service == "Republish") {
      message.body = detail::DecodeRepublishRequest(body);
    } else if (service == "TransferSubscriptions") {
      message.body = detail::DecodeTransferSubscriptionsRequest(body);
    } else if (service == "CreateMonitoredItems") {
      message.body = detail::DecodeCreateMonitoredItemsRequest(body);
    } else if (service == "ModifyMonitoredItems") {
      message.body = detail::DecodeModifyMonitoredItemsRequest(body);
    } else if (service == "DeleteMonitoredItems") {
      message.body = detail::DecodeDeleteMonitoredItemsRequest(body);
    } else if (service == "SetMonitoringMode") {
      message.body = detail::DecodeSetMonitoringModeRequest(body);
    } else {
      const object service_json{{"service", service}, {"body", body}};
      auto decoded = DecodeServiceRequest(service_json);
      if (!decoded.ok())
        return decoded.status();
      message.body = std::visit(
          [](auto&& typed_request) -> RequestBody {
            return std::forward<decltype(typed_request)>(typed_request);
          },
          *decoded);
    }
    return message;
  } catch (...) {
    return Status{StatusCode::Bad_TypeMismatch};
  }
}

StatusOr<ResponseMessage> DecodeResponseMessage(
    const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    const auto service = RequireString(RequireField(obj, "service"));
    ResponseMessage message{
        .request_handle = static_cast<UInt32>(
            RequireUInt64(RequireField(obj, "requestHandle"))),
        .body = CloseSessionResponse{},
    };
    if (service == "FindServers") {
      message.body = DecodeFindServersResponse(body);
    } else if (service == "GetEndpoints") {
      message.body = DecodeGetEndpointsResponse(body);
    } else if (service == "CreateSession") {
      message.body = DecodeCreateSessionResponse(body);
    } else if (service == "ActivateSession") {
      message.body = DecodeActivateSessionResponse(body);
    } else if (service == "CloseSession") {
      message.body = DecodeCloseSessionResponse(body);
    } else if (service == "CreateSubscription") {
      message.body = detail::DecodeCreateSubscriptionResponse(body);
    } else if (service == "ModifySubscription") {
      message.body = detail::DecodeModifySubscriptionResponse(body);
    } else if (service == "SetPublishingMode") {
      message.body = detail::DecodeSetPublishingModeResponse(body);
    } else if (service == "DeleteSubscriptions") {
      message.body = detail::DecodeDeleteSubscriptionsResponse(body);
    } else if (service == "Publish") {
      message.body = detail::DecodePublishResponse(body);
    } else if (service == "Republish") {
      message.body = detail::DecodeRepublishResponse(body);
    } else if (service == "TransferSubscriptions") {
      message.body = detail::DecodeTransferSubscriptionsResponse(body);
    } else if (service == "CreateMonitoredItems") {
      message.body = detail::DecodeCreateMonitoredItemsResponse(body);
    } else if (service == "ModifyMonitoredItems") {
      message.body = detail::DecodeModifyMonitoredItemsResponse(body);
    } else if (service == "DeleteMonitoredItems") {
      message.body = detail::DecodeDeleteMonitoredItemsResponse(body);
    } else if (service == "SetMonitoringMode") {
      message.body = detail::DecodeSetMonitoringModeResponse(body);
    } else if (service == "ServiceFault") {
      message.body = DecodeServiceFault(body);
    } else {
      const object service_json{{"service", service}, {"body", body}};
      auto decoded = DecodeServiceResponse(service_json);
      if (!decoded.ok())
        return decoded.status();
      message.body = std::visit(
          [](auto&& typed_response) -> ResponseBody {
            return std::forward<decltype(typed_response)>(typed_response);
          },
          *decoded);
    }
    return message;
  } catch (...) {
    return Status{StatusCode::Bad_TypeMismatch};
  }
}

}  // namespace opcua::ws
