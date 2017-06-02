#pragma once

#include "opcua/expanded_node_id.h"
#include "opcua/node_id.h"
#include "opcua/types.h"
#include "opcua/structs.h"
#include "opcua/variant.h"

#include <vector>

namespace opcua {
namespace server {

struct ReferenceState {
  NodeId reference_type_id;
  Boolean inverse;
  ExpandedNodeId target_id;
};

struct NodeState {
  NodeId node_id;
  NodeClass node_class;
  NodeId type_definition_id;
  NodeId parent_id;
  NodeId reference_type_id;
  NodeId super_type_id;
  std::vector<ReferenceState> references;
  std::vector<NodeState> children;
  QualifiedName browse_name;
  LocalizedText display_name;
  Variant value;
  NodeId data_type_id;
};

} // namespace server
} // namespace opcua
