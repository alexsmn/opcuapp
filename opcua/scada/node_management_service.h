#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/base/struct_writer.h"
#include "opcua/scada/node_attributes.h"
#include "opcua/scada/node_class.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <utility>
#include <vector>

namespace opcua {
namespace scada {

struct AddNodesItem {
  NodeId requested_id;
  NodeId parent_id;
  NodeClass node_class = NodeClass::Object;
  NodeId type_definition_id;
  NodeAttributes attributes;
};

struct AddNodesResult {
  StatusCode status_code = StatusCode::Good;
  NodeId added_node_id;
};

struct DeleteNodesItem {
  NodeId node_id;
  bool delete_target_references = false;
};

struct AddReferencesItem {
  NodeId source_node_id;
  NodeId reference_type_id;
  bool forward = true;
  String target_server_uri;
  ExpandedNodeId target_node_id;
  NodeClass target_node_class = NodeClass::Object;
};

struct DeleteReferencesItem {
  NodeId source_node_id;
  NodeId reference_type_id;
  bool forward = true;
  ExpandedNodeId target_node_id;
  bool delete_bidirectional = true;
};

class NodeManagementService {
 public:
  virtual ~NodeManagementService() = default;

  virtual Awaitable<StatusOr<std::vector<AddNodesResult>>> AddNodes(
      std::vector<AddNodesItem> inputs) = 0;

  // Delete record from table. If |return_dependencies| is true and deletion
  // fails, it gets list of related records, which must be deleted before.
  virtual Awaitable<StatusOr<std::vector<StatusCode>>> DeleteNodes(
      std::vector<DeleteNodesItem> inputs) = 0;

  virtual Awaitable<StatusOr<std::vector<StatusCode>>>
  AddReferences(std::vector<AddReferencesItem> inputs) = 0;

  virtual Awaitable<StatusOr<std::vector<StatusCode>>>
  DeleteReferences(std::vector<DeleteReferencesItem> inputs) = 0;
};

inline bool operator==(const AddNodesItem& a, const AddNodesItem& b) {
  return a.requested_id == b.requested_id && a.parent_id == b.parent_id &&
         a.node_class == b.node_class &&
         a.type_definition_id == b.type_definition_id &&
         a.attributes == b.attributes;
}

inline std::ostream& operator<<(std::ostream& stream,
                                const AddNodesItem& item) {
  StructWriter{stream}
      .AddField("requested_id", item.requested_id)
      .AddField("parent_id", item.parent_id)
      .AddField("node_class", item.node_class)
      .AddField("type_definition_id", item.type_definition_id)
      .AddField("attributes", item.attributes);
  return stream;
}

}  // namespace scada
}  // namespace opcua (vendored)
