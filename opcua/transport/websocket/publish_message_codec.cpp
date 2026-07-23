#include "opcua/transport/websocket/json_codec.h"

#include "opcua/base/time_utils.h"
#include "opcua/session/subscription_conversion.h"
#include "opcua/ua/ua_json_codec.h"

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

// Publish / Republish use the generated, spec-conformant OPC UA JSON codec
// (Part 6 §5.4). The NotificationMessage's NotificationData now travels as a
// conformant ExtensionObject array (previously a custom "Type"-tagged scheme),
// bridged to the hand-written notification union by subscription_conversion.

value EncodePublishRequest(const PublishRequest& request) {
  return ua::EncodeJson(subscription_conversion::ToWire(request));
}

PublishRequest DecodePublishRequest(const value& json) {
  ua::PublishRequest wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodePublishResponse(const PublishResponse& response) {
  return ua::EncodeJson(
      subscription_conversion::ToWire(response, /*json_body=*/true));
}

PublishResponse DecodePublishResponse(const value& json) {
  ua::PublishResponse wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeRepublishRequest(const RepublishRequest& request) {
  return ua::EncodeJson(subscription_conversion::ToWire(request));
}

RepublishRequest DecodeRepublishRequest(const value& json) {
  ua::RepublishRequest wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

value EncodeRepublishResponse(const RepublishResponse& response) {
  return ua::EncodeJson(subscription_conversion::ToWire(response));
}

RepublishResponse DecodeRepublishResponse(const value& json) {
  ua::RepublishResponse wire;
  ua::DecodeJson(json, wire);
  return subscription_conversion::ToManaged(wire);
}

// Migrated to the generated conformant OPC UA JSON codec (Part 6 §5.4). The
// response now carries TransferResult objects (StatusCode +
// AvailableSequenceNumbers), replacing the earlier bare status array.
value EncodeTransferSubscriptionsRequest(
    const ua::TransferSubscriptionsRequest& request) {
  return ua::EncodeJson(request);
}

ua::TransferSubscriptionsRequest DecodeTransferSubscriptionsRequest(
    const value& json) {
  ua::TransferSubscriptionsRequest request;
  ua::DecodeJson(json, request);
  return request;
}

value EncodeTransferSubscriptionsResponse(
    const ua::TransferSubscriptionsResponse& response) {
  return ua::EncodeJson(response);
}

ua::TransferSubscriptionsResponse DecodeTransferSubscriptionsResponse(
    const value& json) {
  ua::TransferSubscriptionsResponse response;
  ua::DecodeJson(json, response);
  return response;
}

}  // namespace opcua::ws::detail
