#pragma once

#include "opcua/scada/attribute_ids.h"
#include "opcua/scada/node_id.h"

namespace opcua::scada {

// The structure is used by attribute and monitored item services.
struct ReadValueId {
  NodeId node_id;
  AttributeId attribute_id = opcua::scada::AttributeId::Value;

  bool operator==(const ReadValueId&) const = default;
};

inline std::ostream& operator<<(std::ostream& stream, const ReadValueId& v) {
  return stream << "{" << v.node_id << ", " << v.attribute_id << "}";
}

}  // namespace opcua::scada
