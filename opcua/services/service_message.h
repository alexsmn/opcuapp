#pragma once

#include "opcua/services/attribute_types.h"
#include "opcua/services/history_types.h"
#include "opcua/services/method_types.h"
#include "opcua/services/node_management_types.h"
#include "opcua/services/view_types.h"
#include "opcua/types/status.h"

#include <variant>
#include <vector>

namespace opcua {

struct ReadRequest {
  std::vector<ReadValueId> inputs;
  // TimestampsToReturn as the raw OPC UA enumeration value (0=Source, 1=Server,
  // 2=Both, 3=Neither). Stored raw to avoid an include cycle with message.h;
  // the handler validates the range and applies it. Defaults to Both.
  UInt32 timestamps_to_return = 2;
};

struct ReadResponse {
  Status status{StatusCode::Good};
  std::vector<DataValue> results;
};

struct WriteRequest {
  std::vector<WriteValue> inputs;
};

struct WriteResponse {
  Status status{StatusCode::Good};
  std::vector<StatusCode> results;
};

struct BrowseRequest {
  size_t requested_max_references_per_node = 0;
  std::vector<BrowseDescription> inputs;
  // ViewDescription.viewId. A null id browses the default view; a non-null id
  // is rejected with Bad_ViewIdUnknown since the server exposes no Views.
  NodeId view_id;
};

struct BrowseResponse {
  Status status{StatusCode::Good};
  std::vector<BrowseResult> results;
};

struct BrowseNextRequest {
  bool release_continuation_points = false;
  std::vector<ByteString> continuation_points;
};

struct BrowseNextResponse {
  Status status{StatusCode::Good};
  std::vector<BrowseResult> results;
};

struct TranslateBrowsePathsRequest {
  std::vector<BrowsePath> inputs;
};

struct TranslateBrowsePathsResponse {
  Status status{StatusCode::Good};
  std::vector<BrowsePathResult> results;
};

struct MethodCallRequest {
  NodeId object_id;
  NodeId method_id;
  std::vector<Variant> arguments;
};

struct MethodCallResult {
  Status status{StatusCode::Good};
  std::vector<StatusCode> input_argument_results;
  std::vector<Variant> output_arguments;
};

struct CallRequest {
  std::vector<MethodCallRequest> methods;
};

struct CallResponse {
  Status status{StatusCode::Good};
  std::vector<MethodCallResult> results;
};

struct HistoryReadRawRequest {
  HistoryReadRawDetails details;
};

struct HistoryReadRawResponse {
  HistoryReadRawResult result;
};

struct HistoryReadEventsRequest {
  HistoryReadEventsDetails details;
};

struct HistoryReadEventsResponse {
  HistoryReadEventsResult result;
};

struct AddNodesRequest {
  std::vector<AddNodesItem> items;
};

struct AddNodesResponse {
  Status status{StatusCode::Good};
  std::vector<AddNodesResult> results;
};

struct DeleteNodesRequest {
  std::vector<DeleteNodesItem> items;
};

struct DeleteNodesResponse {
  Status status{StatusCode::Good};
  std::vector<StatusCode> results;
};

struct AddReferencesRequest {
  std::vector<AddReferencesItem> items;
};

struct AddReferencesResponse {
  Status status{StatusCode::Good};
  std::vector<StatusCode> results;
};

struct DeleteReferencesRequest {
  std::vector<DeleteReferencesItem> items;
};

struct DeleteReferencesResponse {
  Status status{StatusCode::Good};
  std::vector<StatusCode> results;
};

using ServiceRequest = std::variant<ReadRequest,
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

using ServiceResponse = std::variant<ReadResponse,
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
