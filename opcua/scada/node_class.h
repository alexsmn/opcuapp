#pragma once

#include <cassert>
#include <ostream>
#include <string>

namespace opcua {

enum class NodeClass {
  Unspecified = 0,
  Object = 1,
  Variable = 2,
  Method = 4,
  ObjectType = 8,
  VariableType = 16,
  ReferenceType = 32,
  DataType = 64,
  View = 128,
};


inline std::string ToString(opcua::NodeClass node_class) {
  switch (node_class) {
    case opcua::NodeClass::Unspecified:
      return "Unspecified";
    case opcua::NodeClass::Object:
      return "Object";
    case opcua::NodeClass::Variable:
      return "Variable";
    case opcua::NodeClass::Method:
      return "Method";
    case opcua::NodeClass::ObjectType:
      return "ObjectType";
    case opcua::NodeClass::VariableType:
      return "VariableType";
    case opcua::NodeClass::ReferenceType:
      return "ReferenceType";
    case opcua::NodeClass::DataType:
      return "DataType";
    case opcua::NodeClass::View:
      return "View";
    default:
      assert(false);
      return "Unknown";
  };
}


inline std::ostream& operator<<(std::ostream& stream, NodeClass node_class) {
  return stream << ToString(node_class);
}

}  // namespace opcua (vendored)
