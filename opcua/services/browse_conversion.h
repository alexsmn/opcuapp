#pragma once

#include "opcua/services/view_types.h"
#include "opcua/ua/ua_types.h"

// Converts between the hand-written Browse vocabulary the view-service callback
// and the continuation-point paging speak (opcua::BrowseDescription,
// opcua::ReferenceDescription, opcua::BrowseResult) and the generated ua::
// types the wire carries. Option A keeps the callback/bridge types untouched
// and converts at the ServiceHandler and client boundaries. OPC UA Part 4 §5.8
// Browse, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8

namespace opcua {

// BrowseDescription: the enum member is spelled `direction` in the hand-written
// type and `browse_direction` in the generated one; the two BrowseDirection
// enums share their integer values.
BrowseDescription ToHandWritten(const ua::BrowseDescription& description);
ua::BrowseDescription ToGenerated(const BrowseDescription& description);

// ReferenceDescription: `forward`<->`is_forward`, and the generated type widens
// node_id/type_definition to ExpandedNodeId; the two NodeClass enums share
// their integer values.
ReferenceDescription ToHandWritten(const ua::ReferenceDescription& reference);
ua::ReferenceDescription ToGenerated(const ReferenceDescription& reference);

// BrowseResult: the hand-written status_code is a StatusCode, the generated one
// a Status.
BrowseResult ToHandWritten(const ua::BrowseResult& result);
ua::BrowseResult ToGenerated(const BrowseResult& result);

}  // namespace opcua
