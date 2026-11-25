#include "opcua/types/attribute_ids.h"

#include <iterator>
#include <string>

namespace opcua {
std::string ToString(opcua::AttributeId attribute_id) {
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
                static_cast<size_t>(opcua::AttributeId::Count));
  // Attribute ids arrive from clients, and OPC UA defines ids past the end of
  // this table (AccessLevelEx and AccessRestrictions, for instance), so an
  // unchecked index read out of bounds whenever one was logged.
  const auto index = static_cast<size_t>(attribute_id);
  if (index >= std::size(kStrings))
    return "AttributeId(" +
           std::to_string(static_cast<long long>(attribute_id)) + ")";
  return kStrings[index];
}
}  // namespace opcua
