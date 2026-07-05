#pragma once

#include "opcua/message.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opcua::binary {

struct ServiceRequestHeader {
  NodeId authentication_token;
  std::uint32_t request_handle = 0;
  // Optional W3C traceparent (https://www.w3.org/TR/trace-context/) for
  // cross-process trace propagation; empty = absent. Carried on the wire in
  // RequestHeader.additionalHeader as an AdditionalParametersType
  // {"traceparent": String} entry. OPC UA Part 4 §7.33 RequestHeader,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
  std::string trace_parent;
};

struct DecodedRequest {
  ServiceRequestHeader header;
  RequestBody body;
  std::vector<std::vector<std::string>> history_event_field_paths;
};

std::optional<std::vector<char>> EncodeServiceRequest(
    const ServiceRequestHeader& header,
    const RequestBody& request);

std::optional<DecodedRequest> DecodeServiceRequest(
    const std::vector<char>& payload);

// Parses just the request envelope + RequestHeader to recover the request
// handle, even when the service is not decodable (unknown/unsupported encoding
// id). Used to answer with a ServiceFault instead of dropping the channel.
// Returns std::nullopt if the envelope/header is malformed.
std::optional<std::uint32_t> DecodeRequestHandle(
    const std::vector<char>& payload);

std::optional<std::vector<char>> EncodeServiceResponse(
    std::uint32_t request_handle,
    const ResponseBody& response);

std::optional<std::vector<char>> EncodeHistoryReadEventsResponse(
    std::uint32_t request_handle,
    const HistoryReadEventsResponse& response,
    std::span<const std::vector<std::string>> field_paths);

// Client-side inverse of EncodeServiceResponse: decodes the body
// that the server produced on the wire into a typed ResponseBody plus
// the originating request_handle. Returns std::nullopt for malformed or
// unsupported response encodings.
struct DecodedResponse {
  std::uint32_t request_handle = 0;
  ResponseBody body;
};

std::optional<DecodedResponse> DecodeServiceResponse(
    const std::vector<char>& payload);

}  // namespace opcua::binary
