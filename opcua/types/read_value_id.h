#pragma once

#include "opcua/types/attribute_ids.h"
#include "opcua/types/node_id.h"

namespace opcua {

// Identifies a Node and Attribute (and optionally an index range / data
// encoding) to read or monitor; used by attribute and monitored-item services.
// OPC UA Part 4 §7.28 ReadValueId,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.28
struct ReadValueId {
  NodeId node_id;
  AttributeId attribute_id = opcua::AttributeId::Value;

  bool operator==(const ReadValueId&) const = default;
};

inline std::ostream& operator<<(std::ostream& stream, const ReadValueId& v) {
  return stream << "{" << v.node_id << ", " << v.attribute_id << "}";
}

}  // namespace opcua
