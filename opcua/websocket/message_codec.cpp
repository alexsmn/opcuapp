#include "opcua/websocket/json_codec.h"

#include "opcua/base/utf_convert.h"

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
    const SetPublishingModeRequest& request);
SetPublishingModeRequest DecodeSetPublishingModeRequest(
    const boost::json::value& json);
boost::json::value EncodeSetPublishingModeResponse(
    const SetPublishingModeResponse& response);
SetPublishingModeResponse DecodeSetPublishingModeResponse(
    const boost::json::value& json);
boost::json::value EncodeDeleteSubscriptionsRequest(
    const DeleteSubscriptionsRequest& request);
DeleteSubscriptionsRequest DecodeDeleteSubscriptionsRequest(
    const boost::json::value& json);
boost::json::value EncodeDeleteSubscriptionsResponse(
    const DeleteSubscriptionsResponse& response);
DeleteSubscriptionsResponse DecodeDeleteSubscriptionsResponse(
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
    const TransferSubscriptionsRequest& request);
TransferSubscriptionsRequest DecodeTransferSubscriptionsRequest(
    const boost::json::value& json);
boost::json::value EncodeTransferSubscriptionsResponse(
    const TransferSubscriptionsResponse& response);
TransferSubscriptionsResponse DecodeTransferSubscriptionsResponse(
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
    const DeleteMonitoredItemsRequest& request);
DeleteMonitoredItemsRequest DecodeDeleteMonitoredItemsRequest(
    const boost::json::value& json);
boost::json::value EncodeDeleteMonitoredItemsResponse(
    const DeleteMonitoredItemsResponse& response);
DeleteMonitoredItemsResponse DecodeDeleteMonitoredItemsResponse(
    const boost::json::value& json);
boost::json::value EncodeSetMonitoringModeRequest(
    const SetMonitoringModeRequest& request);
SetMonitoringModeRequest DecodeSetMonitoringModeRequest(
    const boost::json::value& json);
boost::json::value EncodeSetMonitoringModeResponse(
    const SetMonitoringModeResponse& response);
SetMonitoringModeResponse DecodeSetMonitoringModeResponse(
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

value EncodeNodeId(const scada::NodeId& node_id) {
  return string(node_id.ToString());
}

scada::NodeId DecodeNodeId(const value& json) {
  auto node_id = scada::NodeId::FromString(RequireString(json));
  if (node_id.is_null() && RequireString(json) != "i=0")
    ThrowJsonError("Invalid NodeId");
  return node_id;
}

// OPC UA Part 6 §5.4.2.14: LocalizedText is an object `{ Locale?, Text? }`,
// each field omitted when null/empty. The scada LocalizedText carries text
// only, so we only ever emit `Text`.
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
    const auto raw = RequireUInt64(entry);
    if (raw > std::numeric_limits<unsigned char>::max())
      ThrowJsonError("ByteString element out of range");
    bytes.push_back(static_cast<char>(static_cast<unsigned char>(raw)));
  }
  return bytes;
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

value EncodeCreateSessionRequest(const CreateSessionRequest& request) {
  return object{
      {"RequestedSessionTimeout", request.requested_timeout.InMilliseconds()}};
}

CreateSessionRequest DecodeCreateSessionRequest(const value& json) {
  return {.requested_timeout = base::TimeDelta::FromMilliseconds(RequireInt64(
              RequireField(RequireObject(json), "RequestedSessionTimeout")))};
}

value EncodeCreateSessionResponse(const CreateSessionResponse& response) {
  return object{
      {"Status", EncodeStatus(response.status)},
      {"SessionId", EncodeNodeId(response.session_id)},
      {"AuthenticationToken", EncodeNodeId(response.authentication_token)},
      {"ServerNonce", EncodeByteString(response.server_nonce)},
      {"RevisedSessionTimeout", response.revised_timeout.InMilliseconds()}};
}

CreateSessionResponse DecodeCreateSessionResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .session_id = DecodeNodeId(RequireField(obj, "SessionId")),
          .authentication_token =
              DecodeNodeId(RequireField(obj, "AuthenticationToken")),
          .server_nonce = DecodeByteString(RequireField(obj, "ServerNonce")),
          .revised_timeout = base::TimeDelta::FromMilliseconds(
              RequireInt64(RequireField(obj, "RevisedSessionTimeout")))};
}

value EncodeActivateSessionRequest(const ActivateSessionRequest& request) {
  object json{
      {"SessionId", EncodeNodeId(request.session_id)},
      {"AuthenticationToken", EncodeNodeId(request.authentication_token)},
      {"DeleteExisting", request.delete_existing},
      {"AllowAnonymous", request.allow_anonymous}};
  // UserName / Password are plain strings on the wire — they map onto the
  // UserNameIdentityToken (Part 4 §7.36.4) which types both as String /
  // ByteString, not LocalizedText. The internal storage stays as
  // LocalizedText for now to avoid touching the session manager surface.
  if (request.user_name.has_value())
    json["UserName"] = string(UtfConvert<char>(*request.user_name));
  if (request.password.has_value())
    json["Password"] = string(UtfConvert<char>(*request.password));
  return json;
}

ActivateSessionRequest DecodeActivateSessionRequest(const value& json) {
  const auto& obj = RequireObject(json);
  ActivateSessionRequest request{
      .session_id = DecodeNodeId(RequireField(obj, "SessionId")),
      .authentication_token =
          DecodeNodeId(RequireField(obj, "AuthenticationToken")),
      .delete_existing = RequireBool(RequireField(obj, "DeleteExisting")),
      .allow_anonymous = RequireBool(RequireField(obj, "AllowAnonymous")),
  };
  if (const auto* field = FindField(obj, "UserName"))
    request.user_name =
        UtfConvert<char16_t>(std::string{RequireString(*field)});
  if (const auto* field = FindField(obj, "Password"))
    request.password = UtfConvert<char16_t>(std::string{RequireString(*field)});
  return request;
}

value EncodeActivateSessionResponse(const ActivateSessionResponse& response) {
  object json{{"Status", EncodeStatus(response.status)},
              {"Resumed", response.resumed}};
  if (!response.service_context.user_id().is_null()) {
    json["UserId"] = EncodeNodeId(response.service_context.user_id());
  }
  return json;
}

ActivateSessionResponse DecodeActivateSessionResponse(const value& json) {
  const auto& obj = RequireObject(json);
  return {.status = DecodeStatus(RequireField(obj, "Status")),
          .resumed = RequireBool(RequireField(obj, "Resumed"))};
}

value EncodeCloseSessionRequest(const CloseSessionRequest& request) {
  return object{
      {"SessionId", EncodeNodeId(request.session_id)},
      {"AuthenticationToken", EncodeNodeId(request.authentication_token)}};
}

CloseSessionRequest DecodeCloseSessionRequest(const value& json) {
  const auto& obj = RequireObject(json);
  return {.session_id = DecodeNodeId(RequireField(obj, "SessionId")),
          .authentication_token =
              DecodeNodeId(RequireField(obj, "AuthenticationToken"))};
}

value EncodeCloseSessionResponse(const CloseSessionResponse& response) {
  return object{{"Status", EncodeStatus(response.status)}};
}

CloseSessionResponse DecodeCloseSessionResponse(const value& json) {
  return {.status = DecodeStatus(RequireField(RequireObject(json), "Status"))};
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

value EncodeApplicationDescription(const ApplicationDescription& description) {
  return object{
      {"ApplicationUri", description.application_uri},
      {"ProductUri", description.product_uri},
      {"ApplicationName", EncodeLocalizedText(description.application_name)},
      {"ApplicationType",
       static_cast<std::uint64_t>(description.application_type)},
      {"GatewayServerUri", description.gateway_server_uri},
      {"DiscoveryProfileUri", description.discovery_profile_uri},
      {"DiscoveryUrls", EncodeStringArray(description.discovery_urls)}};
}

ApplicationDescription DecodeApplicationDescription(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .application_uri =
          std::string{RequireString(RequireField(obj, "ApplicationUri"))},
      .product_uri =
          std::string{RequireString(RequireField(obj, "ProductUri"))},
      .application_name =
          DecodeLocalizedText(RequireField(obj, "ApplicationName")),
      .application_type = static_cast<ApplicationType>(
          RequireUInt64(RequireField(obj, "ApplicationType"))),
      .gateway_server_uri =
          std::string{RequireString(RequireField(obj, "GatewayServerUri"))},
      .discovery_profile_uri =
          std::string{RequireString(RequireField(obj, "DiscoveryProfileUri"))},
      .discovery_urls = DecodeStringArray(RequireField(obj, "DiscoveryUrls"))};
}

value EncodeUserTokenPolicy(const UserTokenPolicy& policy) {
  return object{{"PolicyId", policy.policy_id},
                {"TokenType", static_cast<std::uint64_t>(policy.token_type)},
                {"IssuedTokenType", policy.issued_token_type},
                {"IssuerEndpointUrl", policy.issuer_endpoint_url},
                {"SecurityPolicyUri", policy.security_policy_uri}};
}

UserTokenPolicy DecodeUserTokenPolicy(const value& json) {
  const auto& obj = RequireObject(json);
  return {
      .policy_id = std::string{RequireString(RequireField(obj, "PolicyId"))},
      .token_type = static_cast<UserTokenType>(
          RequireUInt64(RequireField(obj, "TokenType"))),
      .issued_token_type =
          std::string{RequireString(RequireField(obj, "IssuedTokenType"))},
      .issuer_endpoint_url =
          std::string{RequireString(RequireField(obj, "IssuerEndpointUrl"))},
      .security_policy_uri =
          std::string{RequireString(RequireField(obj, "SecurityPolicyUri"))}};
}

value EncodeEndpointDescription(const EndpointDescription& endpoint) {
  array tokens;
  for (const auto& token : endpoint.user_identity_tokens)
    tokens.emplace_back(EncodeUserTokenPolicy(token));
  return object{
      {"EndpointUrl", endpoint.endpoint_url},
      {"Server", EncodeApplicationDescription(endpoint.server)},
      {"SecurityMode", static_cast<std::uint64_t>(endpoint.security_mode)},
      {"SecurityPolicyUri", endpoint.security_policy_uri},
      {"UserIdentityTokens", std::move(tokens)},
      {"TransportProfileUri", endpoint.transport_profile_uri},
      {"SecurityLevel", endpoint.security_level}};
}

EndpointDescription DecodeEndpointDescription(const value& json) {
  const auto& obj = RequireObject(json);
  EndpointDescription endpoint{
      .endpoint_url =
          std::string{RequireString(RequireField(obj, "EndpointUrl"))},
      .server = DecodeApplicationDescription(RequireField(obj, "Server")),
      .security_mode = static_cast<MessageSecurityMode>(
          RequireUInt64(RequireField(obj, "SecurityMode"))),
      .security_policy_uri =
          std::string{RequireString(RequireField(obj, "SecurityPolicyUri"))},
      .transport_profile_uri =
          std::string{RequireString(RequireField(obj, "TransportProfileUri"))},
      .security_level = static_cast<scada::UInt8>(
          RequireUInt64(RequireField(obj, "SecurityLevel")))};
  for (const auto& token : RequireField(obj, "UserIdentityTokens").as_array())
    endpoint.user_identity_tokens.push_back(DecodeUserTokenPolicy(token));
  return endpoint;
}

value EncodeFindServersRequest(const FindServersRequest& request) {
  return object{{"EndpointUrl", request.endpoint_url},
                {"LocaleIds", EncodeStringArray(request.locale_ids)},
                {"ServerUris", EncodeStringArray(request.server_uris)}};
}

FindServersRequest DecodeFindServersRequest(const value& json) {
  const auto& obj = RequireObject(json);
  return {.endpoint_url =
              std::string{RequireString(RequireField(obj, "EndpointUrl"))},
          .locale_ids = DecodeStringArray(RequireField(obj, "LocaleIds")),
          .server_uris = DecodeStringArray(RequireField(obj, "ServerUris"))};
}

value EncodeGetEndpointsRequest(const GetEndpointsRequest& request) {
  return object{{"EndpointUrl", request.endpoint_url},
                {"LocaleIds", EncodeStringArray(request.locale_ids)},
                {"ProfileUris", EncodeStringArray(request.profile_uris)}};
}

GetEndpointsRequest DecodeGetEndpointsRequest(const value& json) {
  const auto& obj = RequireObject(json);
  return {.endpoint_url =
              std::string{RequireString(RequireField(obj, "EndpointUrl"))},
          .locale_ids = DecodeStringArray(RequireField(obj, "LocaleIds")),
          .profile_uris = DecodeStringArray(RequireField(obj, "ProfileUris"))};
}

value EncodeFindServersResponse(const FindServersResponse& response) {
  array servers;
  for (const auto& server : response.servers)
    servers.emplace_back(EncodeApplicationDescription(server));
  return object{{"Status", EncodeStatus(response.status)},
                {"Servers", std::move(servers)}};
}

FindServersResponse DecodeFindServersResponse(const value& json) {
  const auto& obj = RequireObject(json);
  FindServersResponse response{.status =
                                   DecodeStatus(RequireField(obj, "Status"))};
  for (const auto& server : RequireField(obj, "Servers").as_array())
    response.servers.push_back(DecodeApplicationDescription(server));
  return response;
}

value EncodeGetEndpointsResponse(const GetEndpointsResponse& response) {
  array endpoints;
  for (const auto& endpoint : response.endpoints)
    endpoints.emplace_back(EncodeEndpointDescription(endpoint));
  return object{{"Status", EncodeStatus(response.status)},
                {"Endpoints", std::move(endpoints)}};
}

GetEndpointsResponse DecodeGetEndpointsResponse(const value& json) {
  const auto& obj = RequireObject(json);
  GetEndpointsResponse response{.status =
                                    DecodeStatus(RequireField(obj, "Status"))};
  for (const auto& endpoint : RequireField(obj, "Endpoints").as_array())
    response.endpoints.push_back(DecodeEndpointDescription(endpoint));
  return response;
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
        } else if constexpr (std::is_same_v<T, SetPublishingModeRequest>) {
          json["service"] = "SetPublishingMode";
          json["body"] = detail::EncodeSetPublishingModeRequest(typed_request);
        } else if constexpr (std::is_same_v<T, DeleteSubscriptionsRequest>) {
          json["service"] = "DeleteSubscriptions";
          json["body"] =
              detail::EncodeDeleteSubscriptionsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, PublishRequest>) {
          json["service"] = "Publish";
          json["body"] = detail::EncodePublishRequest(typed_request);
        } else if constexpr (std::is_same_v<T, RepublishRequest>) {
          json["service"] = "Republish";
          json["body"] = detail::EncodeRepublishRequest(typed_request);
        } else if constexpr (std::is_same_v<T, TransferSubscriptionsRequest>) {
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
        } else if constexpr (std::is_same_v<T, DeleteMonitoredItemsRequest>) {
          json["service"] = "DeleteMonitoredItems";
          json["body"] =
              detail::EncodeDeleteMonitoredItemsRequest(typed_request);
        } else if constexpr (std::is_same_v<T, SetMonitoringModeRequest>) {
          json["service"] = "SetMonitoringMode";
          json["body"] = detail::EncodeSetMonitoringModeRequest(typed_request);
        } else {
          const auto service_value = EncodeJson(ServiceRequest{typed_request});
          const auto& service_json = RequireObject(service_value);
          json["service"] = service_json.at("service");
          json["body"] = service_json.at("body");
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
        } else if constexpr (std::is_same_v<T, SetPublishingModeResponse>) {
          json["service"] = "SetPublishingMode";
          json["body"] =
              detail::EncodeSetPublishingModeResponse(typed_response);
        } else if constexpr (std::is_same_v<T, DeleteSubscriptionsResponse>) {
          json["service"] = "DeleteSubscriptions";
          json["body"] =
              detail::EncodeDeleteSubscriptionsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, PublishResponse>) {
          json["service"] = "Publish";
          json["body"] = detail::EncodePublishResponse(typed_response);
        } else if constexpr (std::is_same_v<T, RepublishResponse>) {
          json["service"] = "Republish";
          json["body"] = detail::EncodeRepublishResponse(typed_response);
        } else if constexpr (std::is_same_v<T, TransferSubscriptionsResponse>) {
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
        } else if constexpr (std::is_same_v<T, DeleteMonitoredItemsResponse>) {
          json["service"] = "DeleteMonitoredItems";
          json["body"] =
              detail::EncodeDeleteMonitoredItemsResponse(typed_response);
        } else if constexpr (std::is_same_v<T, SetMonitoringModeResponse>) {
          json["service"] = "SetMonitoringMode";
          json["body"] =
              detail::EncodeSetMonitoringModeResponse(typed_response);
        } else if constexpr (std::is_same_v<T, ServiceFault>) {
          json["service"] = "ServiceFault";
          json["body"] = EncodeServiceFault(typed_response);
        } else {
          const auto service_value =
              EncodeJson(ServiceResponse{typed_response});
          const auto& service_json = RequireObject(service_value);
          json["service"] = service_json.at("service");
          json["body"] = service_json.at("body");
        }
        return json;
      },
      response.body);
}

scada::StatusOr<RequestMessage> DecodeRequestMessage(
    const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    const auto service = RequireString(RequireField(obj, "service"));
    RequestMessage message{
        .request_handle = static_cast<scada::UInt32>(
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
    return scada::Status{scada::StatusCode::Bad_CantParseString};
  }
}

scada::StatusOr<ResponseMessage> DecodeResponseMessage(
    const boost::json::value& json) {
  try {
    const auto& obj = RequireObject(json);
    const auto& body = RequireField(obj, "body");
    const auto service = RequireString(RequireField(obj, "service"));
    ResponseMessage message{
        .request_handle = static_cast<scada::UInt32>(
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
    return scada::Status{scada::StatusCode::Bad_CantParseString};
  }
}

}  // namespace opcua::ws
