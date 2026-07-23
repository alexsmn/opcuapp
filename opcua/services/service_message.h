#pragma once

#include "opcua/services/attribute_types.h"
#include "opcua/services/history_types.h"
#include "opcua/services/method_types.h"
#include "opcua/services/node_management_types.h"
#include "opcua/services/view_types.h"
#include "opcua/types/status.h"
#include "opcua/ua/ua_types.h"

#include <variant>
#include <vector>

namespace opcua {

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

// A single HistoryUpdate detail. OPC UA carries an array of
// historyUpdateDetails extension objects on one HistoryUpdate service; we
// support the two writer kinds, dispatched by the extension-object type id on
// the wire (UpdateDataDetails 682 / UpdateEventDetails 685).
using HistoryUpdateDetails =
    std::variant<UpdateDataDetails, UpdateEventDetails>;

struct HistoryUpdateRequest {
  HistoryUpdateDetails details;
};

struct HistoryUpdateResponse {
  HistoryUpdateResult result;
};

using ServiceRequest = std::variant<ua::ReadRequest,
                                    ua::WriteRequest,
                                    ua::BrowseRequest,
                                    ua::BrowseNextRequest,
                                    TranslateBrowsePathsRequest,
                                    CallRequest,
                                    HistoryReadRawRequest,
                                    HistoryReadEventsRequest,
                                    HistoryUpdateRequest,
                                    ua::AddNodesRequest,
                                    ua::DeleteNodesRequest,
                                    ua::AddReferencesRequest,
                                    ua::DeleteReferencesRequest>;

using ServiceResponse = std::variant<ua::ReadResponse,
                                     ua::WriteResponse,
                                     ua::BrowseResponse,
                                     ua::BrowseNextResponse,
                                     TranslateBrowsePathsResponse,
                                     CallResponse,
                                     HistoryReadRawResponse,
                                     HistoryReadEventsResponse,
                                     HistoryUpdateResponse,
                                     ua::AddNodesResponse,
                                     ua::DeleteNodesResponse,
                                     ua::AddReferencesResponse,
                                     ua::DeleteReferencesResponse>;

}  // namespace opcua
