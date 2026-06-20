#pragma once

#include "opcua/message.h"

#include <optional>
#include <span>
#include <vector>

namespace opcua::binary {

struct ServiceRequestHeader {
  scada::NodeId authentication_token;
  std::uint32_t request_handle = 0;
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
