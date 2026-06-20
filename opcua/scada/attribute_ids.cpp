#include "opcua/scada/attribute_ids.h"

namespace opcua {
std::string ToString(opcua::scada::AttributeId attribute_id) {
  static const char* kStrings[] = {
      "Unknown",         "NodeId",
      "NodeClass",       "BrowseName",
      "DisplayName",     "Description",
      "WriteMask",       "UserWriteMask",
      "IsAbstract",      "Symmetric",
      "InverseName",     "ContainsNoLoops",
      "EventNotifier",   "Value",
      "DataType",        "ValueRank",
      "ArrayDimensions", "AccessLevel",
      "UserAccessLevel", "MinimumSamplingInterval",
      "Historizing",     "Executable",
      "UserExecutable",
  };
  static_assert(std::size(kStrings) ==
                static_cast<size_t>(opcua::scada::AttributeId::Count));
  return kStrings[static_cast<size_t>(attribute_id)];
}
}  // namespace opcua (vendored)
