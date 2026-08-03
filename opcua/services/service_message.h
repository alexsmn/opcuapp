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

// HistoryRead (raw + events) and HistoryUpdate use the generated, spec-
// conformant ua:: wire messages directly; the raw-vs-events / data-vs-event
// split opcuapp models is decoded at the ServiceHandler / client boundary by
// services/history_conversion.

using ServiceRequest = std::variant<ua::ReadRequest,
                                    ua::WriteRequest,
                                    ua::BrowseRequest,
                                    ua::BrowseNextRequest,
                                    ua::TranslateBrowsePathsToNodeIdsRequest,
                                    ua::CallRequest,
                                    ua::HistoryReadRequest,
                                    ua::HistoryUpdateRequest,
                                    ua::AddNodesRequest,
                                    ua::DeleteNodesRequest,
                                    ua::AddReferencesRequest,
                                    ua::DeleteReferencesRequest>;

using ServiceResponse = std::variant<ua::ReadResponse,
                                     ua::WriteResponse,
                                     ua::BrowseResponse,
                                     ua::BrowseNextResponse,
                                     ua::TranslateBrowsePathsToNodeIdsResponse,
                                     ua::CallResponse,
                                     ua::HistoryReadResponse,
                                     ua::HistoryUpdateResponse,
                                     ua::AddNodesResponse,
                                     ua::DeleteNodesResponse,
                                     ua::AddReferencesResponse,
                                     ua::DeleteReferencesResponse>;

}  // namespace opcua
