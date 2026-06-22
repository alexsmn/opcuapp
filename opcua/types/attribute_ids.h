#pragma once

#include <ostream>
#include <string>

namespace opcua {

// Numeric identifiers of the standard Node Attributes (NodeId, NodeClass,
// BrowseName, Value, etc.) addressed by the Attribute services. The Attribute
// concept is defined in OPC UA Part 3 §5 Address Space concepts,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/5 (numeric ids per
// OPC UA Part 6 §A.1).
enum class AttributeId {
  NodeId = 1,
  NodeClass = 2,
  BrowseName = 3,
  DisplayName = 4,
  Description = 5,
  WriteMask = 6,
  UserWriteMask = 7,
  IsAbstract = 8,
  Symmetric = 9,
  InverseName = 10,
  ContainsNoLoops = 11,
  EventNotifier = 12,
  Value = 13,
  DataType = 14,
  ValueRank = 15,
  ArrayDimensions = 16,
  AccessLevel = 17,
  UserAccessLevel = 18,
  MinimumSamplingInterval = 19,
  Historizing = 20,
  Executable = 21,
  UserExecutable = 22,
  Count,
};

std::string ToString(opcua::AttributeId attribute_id);

inline std::ostream& operator<<(std::ostream& stream,
                                opcua::AttributeId attribute_id) {
  return stream << ToString(attribute_id);
}

}  // namespace opcua
