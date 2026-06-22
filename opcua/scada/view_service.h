#pragma once

#include "opcua/scada/expanded_node_id.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/node_class.h"
#include "opcua/scada/qualified_name.h"
#include "opcua/scada/status.h"

#include <ostream>
#include <tuple>
#include <vector>

namespace opcua {

enum class BrowseDirection { Forward = 0, Inverse = 1, Both = 2 };

// OPC UA Part 4 §7.3 BrowseResultMask bit values, selecting which
// ReferenceDescription fields a Browse returns.
enum BrowseResultMask : UInt32 {
  kBrowseResultReferenceType = 0x01,
  kBrowseResultIsForward = 0x02,
  kBrowseResultNodeClass = 0x04,
  kBrowseResultBrowseName = 0x08,
  kBrowseResultDisplayName = 0x10,
  kBrowseResultTypeDefinition = 0x20,
};

struct BrowseDescription {
  bool operator==(const BrowseDescription&) const = default;

  NodeId node_id;
  BrowseDirection direction = BrowseDirection::Both;
  NodeId reference_type_id;
  bool include_subtypes = true;
  // OPC UA Part 4 §7.3 BrowseDescription.nodeClassMask: a bitmask of NodeClass
  // values; only references to target nodes of a listed class are returned.
  // 0 means return references to nodes of any class.
  UInt32 node_class_mask = 0;
  // OPC UA Part 4 §7.3 BrowseDescription.resultMask: a bitmask selecting which
  // ReferenceDescription fields to populate (ReferenceType=1, IsForward=2,
  // NodeClass=4, BrowseName=8, DisplayName=16, TypeDefinition=32). Defaults to
  // all fields so internal callers get fully populated results.
  UInt32 result_mask = 0x3F;
};

struct ReferenceDescription {
  NodeId reference_type_id;
  bool forward = true;
  NodeId node_id;
  NodeClass node_class = NodeClass::Unspecified;
  // OPC UA Part 4 §7.29 ReferenceDescription: the target node's BrowseName,
  // DisplayName, and (for Object/Variable targets) its TypeDefinition. Returned
  // when requested via BrowseDescription.resultMask.
  QualifiedName browse_name;
  LocalizedText display_name;
  NodeId type_definition;

  // Ordered by the identity fields (enough to key references in a std::set;
  // QualifiedName has no operator<=> so it cannot participate in the default
  // ordering). Equality compares every field.
  auto operator<=>(const ReferenceDescription& other) const {
    return std::tie(reference_type_id, forward, node_id, node_class,
                    type_definition) <=>
           std::tie(other.reference_type_id, other.forward, other.node_id,
                    other.node_class, other.type_definition);
  }
  bool operator==(const ReferenceDescription& other) const {
    return reference_type_id == other.reference_type_id &&
           forward == other.forward && node_id == other.node_id &&
           node_class == other.node_class && browse_name == other.browse_name &&
           display_name == other.display_name &&
           type_definition == other.type_definition;
  }
};

using ReferenceDescriptions = std::vector<ReferenceDescription>;

struct BrowseResult {
  bool operator==(const BrowseResult&) const = default;

  StatusCode status_code = StatusCode::Good;
  ByteString continuation_point;
  ReferenceDescriptions references;
};

struct RelativePathElement {
  bool operator==(const RelativePathElement&) const = default;

  NodeId reference_type_id;
  bool inverse = false;
  bool include_subtypes = true;
  QualifiedName target_name;
};

using RelativePath = std::vector<RelativePathElement>;

struct BrowsePath {
  bool operator==(const BrowsePath&) const = default;

  // Must be named |node_id| for generalization.
  NodeId node_id;
  RelativePath relative_path;
};

struct BrowsePathTarget {
  ExpandedNodeId target_id;
  size_t remaining_path_index = 0;
};

// TODO: Replace with `StatusOr`.
struct BrowsePathResult {
  StatusCode status_code = StatusCode::Good;
  std::vector<BrowsePathTarget> targets;
};

std::ostream& operator<<(std::ostream& stream, BrowseDirection v);
std::ostream& operator<<(std::ostream& stream, const BrowseDescription& v);
std::ostream& operator<<(std::ostream& stream, const ReferenceDescription& v);
std::ostream& operator<<(std::ostream& stream, const BrowseResult& v);
std::ostream& operator<<(std::ostream& stream, const RelativePathElement& v);
std::ostream& operator<<(std::ostream& stream, const BrowsePath& v);
std::ostream& operator<<(std::ostream& stream, const BrowsePathTarget& v);
std::ostream& operator<<(std::ostream& stream, const BrowsePathResult& v);

}  // namespace opcua
