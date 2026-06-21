#pragma once

#include "opcua/scada/attribute_service.h"
#include "opcua/scada/history_types.h"
#include "opcua/scada/method_service.h"
#include "opcua/scada/node_management_service.h"
#include "opcua/scada/status.h"
#include "opcua/scada/view_service.h"

#include <variant>
#include <vector>

namespace opcua {

struct ReadRequest {
  std::vector<scada::ReadValueId> inputs;
  // TimestampsToReturn as the raw OPC UA enumeration value (0=Source, 1=Server,
  // 2=Both, 3=Neither). Stored raw to avoid an include cycle with message.h;
  // the handler validates the range and applies it. Defaults to Both.
  scada::UInt32 timestamps_to_return = 2;
};

struct ReadResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::DataValue> results;
};

struct WriteRequest {
  std::vector<scada::WriteValue> inputs;
};

struct WriteResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct BrowseRequest {
  size_t requested_max_references_per_node = 0;
  std::vector<scada::BrowseDescription> inputs;
};

struct BrowseResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::BrowseResult> results;
};

struct BrowseNextRequest {
  bool release_continuation_points = false;
  std::vector<scada::ByteString> continuation_points;
};

struct BrowseNextResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::BrowseResult> results;
};

struct TranslateBrowsePathsRequest {
  std::vector<scada::BrowsePath> inputs;
};

struct TranslateBrowsePathsResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::BrowsePathResult> results;
};

struct MethodCallRequest {
  scada::NodeId object_id;
  scada::NodeId method_id;
  std::vector<scada::Variant> arguments;
};

struct MethodCallResult {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> input_argument_results;
  std::vector<scada::Variant> output_arguments;
};

struct CallRequest {
  std::vector<MethodCallRequest> methods;
};

struct CallResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<MethodCallResult> results;
};

struct HistoryReadRawRequest {
  scada::HistoryReadRawDetails details;
};

struct HistoryReadRawResponse {
  scada::HistoryReadRawResult result;
};

struct HistoryReadEventsRequest {
  scada::HistoryReadEventsDetails details;
};

struct HistoryReadEventsResponse {
  scada::HistoryReadEventsResult result;
};

struct AddNodesRequest {
  std::vector<scada::AddNodesItem> items;
};

struct AddNodesResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::AddNodesResult> results;
};

struct DeleteNodesRequest {
  std::vector<scada::DeleteNodesItem> items;
};

struct DeleteNodesResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct AddReferencesRequest {
  std::vector<scada::AddReferencesItem> items;
};

struct AddReferencesResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

struct DeleteReferencesRequest {
  std::vector<scada::DeleteReferencesItem> items;
};

struct DeleteReferencesResponse {
  scada::Status status{scada::StatusCode::Good};
  std::vector<scada::StatusCode> results;
};

using ServiceRequest =
    std::variant<ReadRequest,
                 WriteRequest,
                 BrowseRequest,
                 BrowseNextRequest,
                 TranslateBrowsePathsRequest,
                 CallRequest,
                 HistoryReadRawRequest,
                 HistoryReadEventsRequest,
                 AddNodesRequest,
                 DeleteNodesRequest,
                 AddReferencesRequest,
                 DeleteReferencesRequest>;

using ServiceResponse =
    std::variant<ReadResponse,
                 WriteResponse,
                 BrowseResponse,
                 BrowseNextResponse,
                 TranslateBrowsePathsResponse,
                 CallResponse,
                 HistoryReadRawResponse,
                 HistoryReadEventsResponse,
                 AddNodesResponse,
                 DeleteNodesResponse,
                 AddReferencesResponse,
                 DeleteReferencesResponse>;

}  // namespace opcua
