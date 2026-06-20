#pragma once

#include <ostream>
#include <string>

namespace opcua::scada {

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

}  // namespace opcua::scada

std::string ToString(opcua::scada::AttributeId attribute_id);

namespace opcua::scada {

inline std::ostream& operator<<(std::ostream& stream,
                                opcua::scada::AttributeId attribute_id) {
  return stream << ToString(attribute_id);
}

}  // namespace opcua::scada
