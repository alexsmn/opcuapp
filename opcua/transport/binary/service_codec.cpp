#include "opcua/transport/binary/service_codec.h"

#include "opcua/events/event_filter.h"
#include "opcua/session/discovery_conversion.h"
#include "opcua/session/session_conversion.h"
#include "opcua/session/subscription_conversion.h"
#include "opcua/transport/binary/codec_utils.h"

#include "opcua/types/localized_text.h"
#include "opcua/types/standard_node_ids.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_service_header.h"

#include <boost/json.hpp>

namespace opcua::binary {
namespace {

// AdditionalParametersType Default Binary encoding id
// (AdditionalParametersType_Encoding_DefaultBinary, i=17537; the DataType is
// i=16313). Layout per Opc.Ua.Types.bsd.xml: Int32 NoOfParameters followed by
// inline KeyValuePair{QualifiedName key; Variant value} structures. Used as
// the RequestHeader.additionalHeader carrier for the W3C traceparent.
// OPC UA Part 4 §7.33 RequestHeader,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
constexpr std::uint32_t kAdditionalParametersTypeEncodingId = 17537;

constexpr std::string_view kTraceParentParameterName = "traceparent";

// True when an array element count is larger than the bytes left to decode.
// Every encoded element occupies at least one byte, so a larger count is
// malformed; rejecting it bounds the resize/reserve against a decode bomb.
// OPC UA Part 6 §5.1.2 Decoding Errors,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2
bool ArrayCountExceedsRemaining(const Decoder& decoder, std::int32_t count) {
  return count < 0 ||
         static_cast<std::size_t>(count) > decoder.remaining().size();
}

bool SkipString(Decoder& decoder) {
  std::int32_t length = 0;
  if (!decoder.Decode(length)) {
    return false;
  }
  if (length < 0) {
    return true;
  }
  return decoder.Skip(static_cast<std::size_t>(length));
}

// Extracts the "traceparent" entry from a decoded
// RequestHeader.additionalHeader. Tolerant by design: the additionalHeader is
// a vendor extension slot, so unknown extension types and any malformed body
// content yield an empty result instead of a decode failure — data from the
// wire must never fail the request over an optional header. Only the
// ExtensionObject envelope (already decoded by the caller) stays strict.
std::string ReadTraceParentFromAdditionalHeader(
    const DecodedExtensionObject& additional) {
  if (additional.type_id != kAdditionalParametersTypeEncodingId ||
      additional.encoding != 0x01) {
    return {};
  }

  Decoder decoder{additional.body};
  std::int32_t count = 0;
  if (!decoder.Decode(count) || count < 0 ||
      ArrayCountExceedsRemaining(decoder, count)) {
    return {};
  }

  for (std::int32_t i = 0; i < count; ++i) {
    QualifiedName key;
    Variant value;
    if (!decoder.Decode(key) || !decoder.Decode(value)) {
      return {};
    }
    if (key.namespace_index() == 0 && key.name() == kTraceParentParameterName) {
      if (const String* text = value.get_if<String>()) {
        return *text;
      }
      return {};  // A non-String "traceparent" value is dropped.
    }
  }
  return {};
}

bool ReadRequestHeader(Decoder& decoder, ServiceRequestHeader& header) {
  std::int64_t ignored_timestamp = 0;
  if (!decoder.Decode(header.authentication_token) ||
      !decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(header.request_handle)) {
    return false;
  }

  std::uint32_t ignored_u32 = 0;
  if (!decoder.Decode(ignored_u32) || !SkipString(decoder) ||
      !decoder.Decode(ignored_u32)) {
    return false;
  }

  DecodedExtensionObject additional;
  if (!decoder.Decode(additional)) {
    return false;
  }
  header.trace_parent = ReadTraceParentFromAdditionalHeader(additional);
  return true;
}

// -- Response decode helpers (client-side inverses of Append*/Encode*) -------

struct DecodedResponseHeader {
  std::uint32_t request_handle = 0;
  Status service_result{StatusCode::Good};
};

bool ReadResponseHeader(Decoder& decoder, DecodedResponseHeader& header) {
  std::int64_t ignored_timestamp = 0;
  std::uint32_t status_word = 0;
  std::uint8_t ignored_diagnostics_mask = 0;
  std::int32_t ignored_string_table_count = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(header.request_handle) || !decoder.Decode(status_word) ||
      !decoder.Decode(ignored_diagnostics_mask) ||
      !decoder.Decode(ignored_string_table_count)) {
    return false;
  }
  header.service_result = Status::FromFullCode(status_word);

  // Additional header is an ExtensionObject; skip it.
  DecodedExtensionObject ignored_additional;
  return decoder.Decode(ignored_additional);
}

// After every response payload, the encoder writes a trailing
// `std::int32_t{-1}` to signal "no diagnostic info array". Skip it.
bool SkipTrailingDiagnosticInfo(Decoder& decoder) {
  std::int32_t sentinel = 0;
  if (!decoder.Decode(sentinel)) {
    return false;
  }
  return sentinel == -1 || sentinel == 0;
}

// -- Decode*Response helpers -------------------------------------------------

// Each returns a populated DecodedResponse on success. They operate
// on the payload inside the ExtensionObject wrapper (the caller strips the
// wrapper via ReadMessage).

template <class Wire>
std::optional<DecodedResponse> DecodeDiscoveryResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  Wire wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = discovery_conversion::ToManaged(wire)};
}

std::optional<DecodedResponse> DecodeFindServersResponse(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::FindServersResponse>(body);
}

std::optional<DecodedResponse> DecodeGetEndpointsResponse(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::GetEndpointsResponse>(body);
}

std::optional<DecodedResponse> DecodeRegisterServerResponse(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::RegisterServerResponse>(body);
}

std::optional<DecodedResponse> DecodeRegisterServer2Response(
    std::span<const char> body) {
  return DecodeDiscoveryResponse<ua::RegisterServer2Response>(body);
}

std::optional<DecodedResponse> DecodeCreateSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSessionResponse wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = session_conversion::ToManaged(wire)};
}

std::optional<DecodedResponse> DecodeActivateSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ActivateSessionResponse wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = session_conversion::ToManaged(wire)};
}

std::optional<DecodedResponse> DecodeCloseSessionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CloseSessionResponse wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = wire.response_header.request_handle,
                         .body = session_conversion::ToManaged(wire)};
}

// Client-side decode for a response migrated to the generated codec: decode
// the whole message (embedded ResponseHeader + body) and lift the
// request_handle back out for the envelope. The mirror of the migrated
// response-encode arm.
template <class Response>
std::optional<DecodedResponse> DecodeGeneratedResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  Response response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  const std::uint32_t request_handle = response.response_header.request_handle;
  return DecodedResponse{.request_handle = request_handle,
                         .body = std::move(response)};
}

// Decodes a pure-ua request body and lifts its RequestHeader (auth token,
// request handle, traceparent) into the ServiceRequestHeader the runtime uses.
// The mirror of the generic request-encode path.
template <class Request>
std::optional<DecodedRequest> DecodeGeneratedRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  Request request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header, .body = std::move(request)};
}

template <class Response>
std::optional<DecodedResponse> DecodeStatusCodeArrayResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  std::int32_t count = 0;
  if (!ReadResponseHeader(decoder, header) || !decoder.Decode(count)) {
    return std::nullopt;
  }
  Response response{.status = header.service_result};
  if (count < 0) {
    count = 0;
  }
  response.results.resize(static_cast<std::size_t>(count));
  for (auto& status : response.results) {
    std::uint32_t status_word = 0;
    if (!decoder.Decode(status_word)) {
      return std::nullopt;
    }
    status = static_cast<StatusCode>(status_word >> 16);
  }
  if (!SkipTrailingDiagnosticInfo(decoder)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = std::move(response)};
}

std::optional<DecodedResponse> DecodeServiceFault(std::span<const char> body) {
  Decoder decoder{body};
  DecodedResponseHeader header;
  if (!ReadResponseHeader(decoder, header)) {
    return std::nullopt;
  }
  return DecodedResponse{.request_handle = header.request_handle,
                         .body = ServiceFault{.status = header.service_result}};
}

std::optional<DecodedResponse> DecodeCreateSubscriptionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSubscriptionResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeModifySubscriptionResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifySubscriptionResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeCreateMonitoredItemsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateMonitoredItemsResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeModifyMonitoredItemsResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifyMonitoredItemsResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodePublishResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::PublishResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedResponse> DecodeRepublishResponse(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::RepublishResponse response;
  if (!ua::Decode(decoder, response) || !decoder.consumed()) {
    return std::nullopt;
  }
  return DecodedResponse{
      .request_handle = response.response_header.request_handle,
      .body = subscription_conversion::ToManaged(response)};
}

std::optional<DecodedRequest> DecodeCreateSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSessionRequest wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header,
                        .body = session_conversion::ToManaged(wire)};
}

template <class Wire>
std::optional<DecodedRequest> DecodeDiscoveryRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  Wire wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header,
                        .body = discovery_conversion::ToManaged(wire)};
}

std::optional<DecodedRequest> DecodeFindServersRequest(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::FindServersRequest>(body);
}

std::optional<DecodedRequest> DecodeGetEndpointsRequest(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::GetEndpointsRequest>(body);
}

std::optional<DecodedRequest> DecodeRegisterServerRequest(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::RegisterServerRequest>(body);
}

std::optional<DecodedRequest> DecodeRegisterServer2Request(
    std::span<const char> body) {
  return DecodeDiscoveryRequest<ua::RegisterServer2Request>(body);
}

std::optional<DecodedRequest> DecodeActivateSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ActivateSessionRequest wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  auto request = session_conversion::ToManaged(wire);
  if (!request) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header, .body = std::move(*request)};
}

std::optional<DecodedRequest> DecodeCloseSessionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CloseSessionRequest wire;
  if (!ua::Decode(decoder, wire) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = wire.request_header.authentication_token,
      .request_handle = wire.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(wire.request_header)};
  return DecodedRequest{.header = header,
                        .body = session_conversion::ToManaged(wire)};
}

std::optional<DecodedRequest> DecodeCreateSubscriptionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateSubscriptionRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeModifySubscriptionRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifySubscriptionRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

// Rejects an out-of-range TimestampsToReturn / MonitoringMode, as the
// hand-written decoders did, so an invalid value faults at decode rather than
// reaching the handler (the generated decoder reads them as raw Int32).
bool ValidTimestampsToReturn(ua::TimestampsToReturn value) {
  return value == ua::TimestampsToReturn::Source ||
         value == ua::TimestampsToReturn::Server ||
         value == ua::TimestampsToReturn::Both ||
         value == ua::TimestampsToReturn::Neither;
}

bool ValidMonitoringMode(ua::MonitoringMode value) {
  return value == ua::MonitoringMode::Disabled ||
         value == ua::MonitoringMode::Sampling ||
         value == ua::MonitoringMode::Reporting;
}

std::optional<DecodedRequest> DecodeCreateMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::CreateMonitoredItemsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed() ||
      !ValidTimestampsToReturn(request.timestamps_to_return)) {
    return std::nullopt;
  }
  for (const auto& item : request.items_to_create) {
    if (!ValidMonitoringMode(item.monitoring_mode)) {
      return std::nullopt;
    }
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeModifyMonitoredItemsRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::ModifyMonitoredItemsRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed() ||
      !ValidTimestampsToReturn(request.timestamps_to_return)) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodePublishRequest(std::span<const char> body) {
  Decoder decoder{body};
  ua::PublishRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

std::optional<DecodedRequest> DecodeRepublishRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::RepublishRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header,
                        .body = subscription_conversion::ToManaged(request)};
}

}  // namespace

// -- ua:: wire projection -----------------------------------------------------
// ToWireRequest/ToWireResponse map a service body onto its generated ua:: wire
// message: a pure-ua body (identified by its kBinaryEncodingId member) is
// already the wire type, so the constrained template is the identity; the
// domain bodies convert through their module. This lets the Encode/Decode
// dispatch be generic over the encoding id that lives with the ua:: type -- no
// per-service arm for the pure-ua services.

template <class T>
  requires requires { T::kBinaryEncodingId; }
const T& ToWireRequest(const T& body) {
  return body;
}
ua::FindServersRequest ToWireRequest(const FindServersRequest& body) {
  return discovery_conversion::ToWire(body);
}
ua::GetEndpointsRequest ToWireRequest(const GetEndpointsRequest& body) {
  return discovery_conversion::ToWire(body);
}
ua::RegisterServerRequest ToWireRequest(const RegisterServerRequest& body) {
  return discovery_conversion::ToWire(body);
}
ua::RegisterServer2Request ToWireRequest(const RegisterServer2Request& body) {
  return discovery_conversion::ToWire(body);
}
ua::CreateSessionRequest ToWireRequest(const CreateSessionRequest& body) {
  return session_conversion::ToWire(body);
}
ua::ActivateSessionRequest ToWireRequest(const ActivateSessionRequest& body) {
  return session_conversion::ToWire(body);
}
ua::CloseSessionRequest ToWireRequest(const CloseSessionRequest& body) {
  return session_conversion::ToWire(body);
}
ua::CreateSubscriptionRequest ToWireRequest(
    const CreateSubscriptionRequest& body) {
  return subscription_conversion::ToWire(body);
}
ua::ModifySubscriptionRequest ToWireRequest(
    const ModifySubscriptionRequest& body) {
  return subscription_conversion::ToWire(body);
}
ua::PublishRequest ToWireRequest(const PublishRequest& body) {
  return subscription_conversion::ToWire(body);
}
ua::RepublishRequest ToWireRequest(const RepublishRequest& body) {
  return subscription_conversion::ToWire(body);
}
ua::CreateMonitoredItemsRequest ToWireRequest(
    const CreateMonitoredItemsRequest& body) {
  return subscription_conversion::ToWire(body);
}
ua::ModifyMonitoredItemsRequest ToWireRequest(
    const ModifyMonitoredItemsRequest& body) {
  return subscription_conversion::ToWire(body);
}

template <class T>
  requires requires { T::kBinaryEncodingId; }
const T& ToWireResponse(const T& body) {
  return body;
}
ua::FindServersResponse ToWireResponse(const FindServersResponse& body) {
  return discovery_conversion::ToWire(body);
}
ua::GetEndpointsResponse ToWireResponse(const GetEndpointsResponse& body) {
  return discovery_conversion::ToWire(body);
}
ua::RegisterServerResponse ToWireResponse(const RegisterServerResponse& body) {
  return discovery_conversion::ToWire(body);
}
ua::RegisterServer2Response ToWireResponse(
    const RegisterServer2Response& body) {
  return discovery_conversion::ToWire(body);
}
ua::CreateSessionResponse ToWireResponse(const CreateSessionResponse& body) {
  return session_conversion::ToWire(body);
}
ua::ActivateSessionResponse ToWireResponse(
    const ActivateSessionResponse& body) {
  return session_conversion::ToWire(body);
}
ua::CloseSessionResponse ToWireResponse(const CloseSessionResponse& body) {
  return session_conversion::ToWire(body);
}
ua::CreateSubscriptionResponse ToWireResponse(
    const CreateSubscriptionResponse& body) {
  return subscription_conversion::ToWire(body);
}
ua::ModifySubscriptionResponse ToWireResponse(
    const ModifySubscriptionResponse& body) {
  return subscription_conversion::ToWire(body);
}
ua::PublishResponse ToWireResponse(const PublishResponse& body) {
  return subscription_conversion::ToWire(body);
}
ua::RepublishResponse ToWireResponse(const RepublishResponse& body) {
  return subscription_conversion::ToWire(body);
}
ua::CreateMonitoredItemsResponse ToWireResponse(
    const CreateMonitoredItemsResponse& body) {
  return subscription_conversion::ToWire(body);
}
ua::ModifyMonitoredItemsResponse ToWireResponse(
    const ModifyMonitoredItemsResponse& body) {
  return subscription_conversion::ToWire(body);
}
ua::ServiceFault ToWireResponse(const ServiceFault& fault) {
  ua::ServiceFault wire;
  wire.response_header.service_result = fault.status;
  return wire;
}

std::optional<std::vector<char>> EncodeServiceRequest(
    const ServiceRequestHeader& header,
    const RequestBody& request) {
  return std::visit(
      [&](const auto& typed_request) -> std::optional<std::vector<char>> {
        auto message = ToWireRequest(typed_request);
        // Stamp the envelope onto the header rather than replacing it: a
        // service body may already have put extension parameters in
        // additionalHeader (the Write service puts its per-item WriteFlags
        // there), and those must survive the transport's own additions.
        ua::ApplyRequestEnvelope(message.request_header,
                                 header.authentication_token,
                                 header.request_handle, header.trace_parent);
        std::vector<char> payload;
        std::vector<char> body;
        Encoder payload_encoder{payload};
        Encoder body_encoder{body};
        ua::Encode(payload_encoder, message);
        AppendMessage(body_encoder,
                      std::decay_t<decltype(message)>::kBinaryEncodingId,
                      payload);
        return body;
      },
      request);
}

std::optional<DecodedRequest> DecodeRegisterNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::RegisterNodesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeUnregisterNodesRequest(
    std::span<const char> body) {
  Decoder decoder{body};
  ua::UnregisterNodesRequest request;
  if (!ua::Decode(decoder, request) || !decoder.consumed()) {
    return std::nullopt;
  }
  ServiceRequestHeader header{
      .authentication_token = request.request_header.authentication_token,
      .request_handle = request.request_header.request_handle,
      .trace_parent = ua::GetTraceParent(request.request_header)};
  return DecodedRequest{.header = header, .body = std::move(request)};
}

std::optional<DecodedRequest> DecodeServiceRequest(
    const std::vector<char>& payload) {
  Decoder decoder{payload};
  const auto message = ReadMessage(decoder);
  if (!message.has_value()) {
    return std::nullopt;
  }

  switch (message->first) {
    case ua::FindServersRequest::kBinaryEncodingId:
      return DecodeFindServersRequest(message->second);
    case ua::GetEndpointsRequest::kBinaryEncodingId:
      return DecodeGetEndpointsRequest(message->second);
    case ua::RegisterServerRequest::kBinaryEncodingId:
      return DecodeRegisterServerRequest(message->second);
    case ua::RegisterServer2Request::kBinaryEncodingId:
      return DecodeRegisterServer2Request(message->second);
    case ua::CreateSessionRequest::kBinaryEncodingId:
      return DecodeCreateSessionRequest(message->second);
    case ua::ActivateSessionRequest::kBinaryEncodingId:
      return DecodeActivateSessionRequest(message->second);
    case ua::CloseSessionRequest::kBinaryEncodingId:
      return DecodeCloseSessionRequest(message->second);
    case ua::CreateSubscriptionRequest::kBinaryEncodingId:
      return DecodeCreateSubscriptionRequest(message->second);
    case ua::ModifySubscriptionRequest::kBinaryEncodingId:
      return DecodeModifySubscriptionRequest(message->second);
    case ua::PublishRequest::kBinaryEncodingId:
      return DecodePublishRequest(message->second);
    case ua::RepublishRequest::kBinaryEncodingId:
      return DecodeRepublishRequest(message->second);
    case ua::CreateMonitoredItemsRequest::kBinaryEncodingId:
      return DecodeCreateMonitoredItemsRequest(message->second);
    case ua::ModifyMonitoredItemsRequest::kBinaryEncodingId:
      return DecodeModifyMonitoredItemsRequest(message->second);
    case ua::ReadRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::ReadRequest>(message->second);
    case ua::WriteRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::WriteRequest>(message->second);
    case ua::BrowseRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::BrowseRequest>(message->second);
    case ua::BrowseNextRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::BrowseNextRequest>(message->second);
    case ua::TranslateBrowsePathsToNodeIdsRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::TranslateBrowsePathsToNodeIdsRequest>(
          message->second);
    case ua::CallRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::CallRequest>(message->second);
    case ua::HistoryReadRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::HistoryReadRequest>(message->second);
    case ua::HistoryUpdateRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::HistoryUpdateRequest>(message->second);
    case ua::AddNodesRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::AddNodesRequest>(message->second);
    case ua::DeleteNodesRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::DeleteNodesRequest>(message->second);
    case ua::AddReferencesRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::AddReferencesRequest>(message->second);
    case ua::DeleteReferencesRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::DeleteReferencesRequest>(
          message->second);
    case ua::RegisterNodesRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::RegisterNodesRequest>(message->second);
    case ua::UnregisterNodesRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::UnregisterNodesRequest>(
          message->second);
    case ua::SetPublishingModeRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::SetPublishingModeRequest>(
          message->second);
    case ua::DeleteSubscriptionsRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::DeleteSubscriptionsRequest>(
          message->second);
    case ua::DeleteMonitoredItemsRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::DeleteMonitoredItemsRequest>(
          message->second);
    case ua::SetMonitoringModeRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::SetMonitoringModeRequest>(
          message->second);
    case ua::TransferSubscriptionsRequest::kBinaryEncodingId:
      return DecodeGeneratedRequest<ua::TransferSubscriptionsRequest>(
          message->second);
    default:
      return std::nullopt;
  }
}

std::optional<std::uint32_t> DecodeRequestHandle(
    const std::vector<char>& payload) {
  Decoder decoder{payload};
  const auto message = ReadMessage(decoder);
  if (!message.has_value())
    return std::nullopt;
  Decoder body{message->second};
  ServiceRequestHeader header;
  if (!ReadRequestHeader(body, header))
    return std::nullopt;
  return header.request_handle;
}

std::optional<DecodedResponse> DecodeServiceResponse(
    const std::vector<char>& payload) {
  Decoder decoder{payload};
  const auto message = ReadMessage(decoder);
  if (!message.has_value()) {
    return std::nullopt;
  }

  switch (message->first) {
    case ua::FindServersResponse::kBinaryEncodingId:
      return DecodeFindServersResponse(message->second);
    case ua::GetEndpointsResponse::kBinaryEncodingId:
      return DecodeGetEndpointsResponse(message->second);
    case ua::RegisterServerResponse::kBinaryEncodingId:
      return DecodeRegisterServerResponse(message->second);
    case ua::RegisterServer2Response::kBinaryEncodingId:
      return DecodeRegisterServer2Response(message->second);
    case ua::CreateSessionResponse::kBinaryEncodingId:
      return DecodeCreateSessionResponse(message->second);
    case ua::ActivateSessionResponse::kBinaryEncodingId:
      return DecodeActivateSessionResponse(message->second);
    case ua::CloseSessionResponse::kBinaryEncodingId:
      return DecodeCloseSessionResponse(message->second);
    case ua::CreateSubscriptionResponse::kBinaryEncodingId:
      return DecodeCreateSubscriptionResponse(message->second);
    case ua::ModifySubscriptionResponse::kBinaryEncodingId:
      return DecodeModifySubscriptionResponse(message->second);
    case ua::CreateMonitoredItemsResponse::kBinaryEncodingId:
      return DecodeCreateMonitoredItemsResponse(message->second);
    case ua::ModifyMonitoredItemsResponse::kBinaryEncodingId:
      return DecodeModifyMonitoredItemsResponse(message->second);
    case ua::PublishResponse::kBinaryEncodingId:
      return DecodePublishResponse(message->second);
    case ua::RepublishResponse::kBinaryEncodingId:
      return DecodeRepublishResponse(message->second);
    case ua::ServiceFault::kBinaryEncodingId:
      return DecodeServiceFault(message->second);
    case ua::ReadResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::ReadResponse>(message->second);
    case ua::WriteResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::WriteResponse>(message->second);
    case ua::BrowseResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::BrowseResponse>(message->second);
    case ua::BrowseNextResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::BrowseNextResponse>(message->second);
    case ua::TranslateBrowsePathsToNodeIdsResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::TranslateBrowsePathsToNodeIdsResponse>(
          message->second);
    case ua::CallResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::CallResponse>(message->second);
    case ua::HistoryReadResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::HistoryReadResponse>(message->second);
    case ua::HistoryUpdateResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::HistoryUpdateResponse>(
          message->second);
    case ua::AddNodesResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::AddNodesResponse>(message->second);
    case ua::DeleteNodesResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::DeleteNodesResponse>(message->second);
    case ua::AddReferencesResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::AddReferencesResponse>(
          message->second);
    case ua::DeleteReferencesResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::DeleteReferencesResponse>(
          message->second);
    case ua::RegisterNodesResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::RegisterNodesResponse>(
          message->second);
    case ua::UnregisterNodesResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::UnregisterNodesResponse>(
          message->second);
    case ua::SetPublishingModeResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::SetPublishingModeResponse>(
          message->second);
    case ua::DeleteSubscriptionsResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::DeleteSubscriptionsResponse>(
          message->second);
    case ua::DeleteMonitoredItemsResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::DeleteMonitoredItemsResponse>(
          message->second);
    case ua::SetMonitoringModeResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::SetMonitoringModeResponse>(
          message->second);
    case ua::TransferSubscriptionsResponse::kBinaryEncodingId:
      return DecodeGeneratedResponse<ua::TransferSubscriptionsResponse>(
          message->second);
    default:
      return std::nullopt;
  }
}

std::optional<std::vector<char>> EncodeServiceResponse(
    std::uint32_t request_handle,
    const ResponseBody& response) {
  return std::visit(
      [&](const auto& typed_response) -> std::optional<std::vector<char>> {
        auto message = ToWireResponse(typed_response);
        message.response_header = ua::MakeResponseHeader(
            request_handle, message.response_header.service_result);
        std::vector<char> payload;
        std::vector<char> body;
        Encoder payload_encoder{payload};
        Encoder body_encoder{body};
        ua::Encode(payload_encoder, message);
        AppendMessage(body_encoder,
                      std::decay_t<decltype(message)>::kBinaryEncodingId,
                      payload);
        return body;
      },
      response);
}

}  // namespace opcua::binary
