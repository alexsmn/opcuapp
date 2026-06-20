#pragma once

#include <cassert>
#include <ostream>
#include <string>

namespace opcua {
namespace scada {

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

inline bool IsTypeDefinition(NodeClass node_class) {
  switch (node_class) {
    case NodeClass::Unspecified:
      return false;
    case NodeClass::DataType:
    case NodeClass::ObjectType:
    case NodeClass::VariableType:
    case NodeClass::ReferenceType:
      return true;
    default:
      return false;
  }
}

inline bool IsInstance(NodeClass node_class) {
  switch (node_class) {
    case NodeClass::Unspecified:
      return false;
    case NodeClass::Object:
    case NodeClass::Variable:
      return true;
    default:
      return false;
  }
}

}  // namespace scada

inline std::string ToString(opcua::scada::NodeClass node_class) {
  switch (node_class) {
    case opcua::scada::NodeClass::Unspecified:
      return "Unspecified";
    case opcua::scada::NodeClass::Object:
      return "Object";
    case opcua::scada::NodeClass::Variable:
      return "Variable";
    case opcua::scada::NodeClass::Method:
      return "Method";
    case opcua::scada::NodeClass::ObjectType:
      return "ObjectType";
    case opcua::scada::NodeClass::VariableType:
      return "VariableType";
    case opcua::scada::NodeClass::ReferenceType:
      return "ReferenceType";
    case opcua::scada::NodeClass::DataType:
      return "DataType";
    case opcua::scada::NodeClass::View:
      return "View";
    default:
      assert(false);
      return "Unknown";
  };
}

namespace scada {

inline std::ostream& operator<<(std::ostream& stream, NodeClass node_class) {
  return stream << ToString(node_class);
}

}  // namespace scada
}  // namespace opcua (vendored)
