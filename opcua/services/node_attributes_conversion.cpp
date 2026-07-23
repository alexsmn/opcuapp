#include "opcua/services/node_attributes_conversion.h"

#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"
#include "opcua/ua/ua_types.h"

#include <boost/json/value.hpp>

#include <any>

namespace opcua {
namespace {

// SpecifiedAttributes bits for the attributes opcua::NodeAttributes can carry.
// OPC UA Part 4 §7.24 NodeAttributesMask,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.24
constexpr UInt32 kDataTypeMask = 16;
constexpr UInt32 kDisplayNameMask = 64;
constexpr UInt32 kValueMask = 2097152;

// Builds the ExtensionObject for a NodeClass whose body carries only the common
// (non-value) attributes opcua::NodeAttributes models.
template <class T>
ExtensionObject BuildCommon(const NodeAttributes& attributes) {
  T body;
  UInt32 mask = 0;
  if (!attributes.display_name.empty()) {
    body.display_name = attributes.display_name;
    mask |= kDisplayNameMask;
  }
  body.specified_attributes = mask;
  return ua::ToExtensionObject(body);
}

// Same, for the value-bearing NodeClasses (Variable, VariableType) whose body
// also carries the data type and value.
template <class T>
ExtensionObject BuildValued(const NodeAttributes& attributes) {
  T body;
  UInt32 mask = 0;
  if (!attributes.display_name.empty()) {
    body.display_name = attributes.display_name;
    mask |= kDisplayNameMask;
  }
  if (!attributes.data_type.is_null()) {
    body.data_type = attributes.data_type;
    mask |= kDataTypeMask;
  }
  if (attributes.value.has_value()) {
    body.value = *attributes.value;
    mask |= kValueMask;
  }
  body.specified_attributes = mask;
  return ua::ToExtensionObject(body);
}

// Decodes a generated *Attributes body out of an ExtensionObject, accepting
// either the binary-body representation (binary transport / JSON UaEncoding=1)
// or an inline JSON body (the form a web client emits).
template <class T>
bool ExtractBody(const ExtensionObject& node_attributes, T& body) {
  if (ua::FromExtensionObject(node_attributes, body))
    return true;
  if (const boost::json::value* json =
          std::any_cast<boost::json::value>(&node_attributes.value())) {
    ua::DecodeJson(*json, body);
    return true;
  }
  return false;
}

template <class T>
NodeAttributes CommonFrom(const ExtensionObject& node_attributes) {
  NodeAttributes attributes;
  T body;
  if (ExtractBody(node_attributes, body))
    attributes.display_name = body.display_name;
  return attributes;
}

template <class T>
NodeAttributes ValuedFrom(const ExtensionObject& node_attributes) {
  NodeAttributes attributes;
  T body;
  if (ExtractBody(node_attributes, body)) {
    attributes.display_name = body.display_name;
    attributes.data_type = body.data_type;
    if (!body.value.is_null())
      attributes.value = body.value;
  }
  return attributes;
}

}  // namespace

ExtensionObject NodeAttributesToExtensionObject(
    NodeClass node_class,
    const NodeAttributes& attributes) {
  switch (node_class) {
    case NodeClass::Variable:
      return BuildValued<ua::VariableAttributes>(attributes);
    case NodeClass::VariableType:
      return BuildValued<ua::VariableTypeAttributes>(attributes);
    case NodeClass::Method:
      return BuildCommon<ua::MethodAttributes>(attributes);
    case NodeClass::ObjectType:
      return BuildCommon<ua::ObjectTypeAttributes>(attributes);
    case NodeClass::ReferenceType:
      return BuildCommon<ua::ReferenceTypeAttributes>(attributes);
    case NodeClass::DataType:
      return BuildCommon<ua::DataTypeAttributes>(attributes);
    case NodeClass::View:
      return BuildCommon<ua::ViewAttributes>(attributes);
    case NodeClass::Object:
    default:
      return BuildCommon<ua::ObjectAttributes>(attributes);
  }
}

NodeAttributes ExtensionObjectToNodeAttributes(
    NodeClass node_class,
    const ExtensionObject& node_attributes) {
  switch (node_class) {
    case NodeClass::Variable:
      return ValuedFrom<ua::VariableAttributes>(node_attributes);
    case NodeClass::VariableType:
      return ValuedFrom<ua::VariableTypeAttributes>(node_attributes);
    case NodeClass::Method:
      return CommonFrom<ua::MethodAttributes>(node_attributes);
    case NodeClass::ObjectType:
      return CommonFrom<ua::ObjectTypeAttributes>(node_attributes);
    case NodeClass::ReferenceType:
      return CommonFrom<ua::ReferenceTypeAttributes>(node_attributes);
    case NodeClass::DataType:
      return CommonFrom<ua::DataTypeAttributes>(node_attributes);
    case NodeClass::View:
      return CommonFrom<ua::ViewAttributes>(node_attributes);
    case NodeClass::Object:
    default:
      return CommonFrom<ua::ObjectAttributes>(node_attributes);
  }
}

}  // namespace opcua
