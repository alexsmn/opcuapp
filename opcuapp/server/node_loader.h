#pragma once

#include <opcuapp/binary_decoder.h>
#include <opcuapp/encodable_type_table.h>
#include <opcuapp/server/node_state.h>
#include <opcuapp/stream.h>
#include <opcuapp/string_table.h>
#include <opcuapp/structs.h>
#include <istream>
#include <vector>

namespace opcua {
namespace server {

opcua::BinaryDecoder::NamespaceMapping MakeNamespaceMapping(
    const StringTable& local,
    const StringTable& global) {
  opcua::BinaryDecoder::NamespaceMapping mapping;
  for (UInt32 local_index = 0; local_index < local.GetCount(); ++local_index) {
    const auto& namespace_name = local[local_index];
    if (OpcUa_String_IsEmpty(&namespace_name))
      continue;
    int global_index = global.FindIndex(namespace_name);
    if (global_index != -1 && local_index != global_index)
      mapping[local_index] = global_index;
  }
  return mapping;
}

struct NodeLoaderContext {
  BinaryDecoder& decoder_;
  std::vector<NodeState>& nodes_;
  const StringTable& namespace_uris_;
};

class NodeLoader : private NodeLoaderContext {
 public:
  explicit NodeLoader(NodeLoaderContext&& context);

  void LoadNodes();

 private:
  enum class AttributesToSave {
    None = 0x00000000,
    AccessLevel = 0x00000001,
    ArrayDimensions = 0x00000002,
    BrowseName = 0x00000004,
    ContainsNoLoops = 0x00000008,
    DataType = 0x00000010,
    Description = 0x00000020,
    DisplayName = 0x00000040,
    EventNotifier = 0x00000080,
    Executable = 0x00000100,
    Historizing = 0x00000200,
    InverseName = 0x00000400,
    IsAbstract = 0x00000800,
    MinimumSamplingInterval = 0x00001000,
    NodeClass = 0x00002000,
    NodeId = 0x00004000,
    Symmetric = 0x00008000,
    UserAccessLevel = 0x00010000,
    UserExecutable = 0x00020000,
    UserWriteMask = 0x00040000,
    ValueRank = 0x00080000,
    WriteMask = 0x00100000,
    Value = 0x00200000,
    SymbolicName = 0x00400000,
    TypeDefinitionId = 0x00800000,
    ModellingRuleId = 0x01000000,
    NumericId = 0x02000000,
    ReferenceTypeId = 0x08000000,
    SuperTypeId = 0x10000000,
    StatusCode = 0x20000000,
  };

  NodeState LoadNode();
  NodeState LoadUnknownNode(unsigned& attribute_mask,
                            NodeClass node_class,
                            String&& symbolic_name,
                            QualifiedName&& browse_name);
  NodeState LoadUnknownChild(unsigned& attribute_mask,
                             NodeClass node_class,
                             String&& symbolic_name,
                             QualifiedName&& browse_name);
  void LoadAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadBaseAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadInstanceAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadObjectAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadTypeAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadReferenceTypeAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadVariableTypeAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadVariableAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadMethodAttributes(unsigned& attribute_mask, NodeState& node);
  void LoadNodeReferences(NodeState& node);
  // Pass by value, since pointers can invalidate when adding new nodes.
  void LoadNodeChildren(NodeState& parent);
  NodeState LoadChild();

  static bool HasAttribute(unsigned& attribute_mask,
                           AttributesToSave attribute_id);
};

void LoadStringTable(BinaryDecoder& decoder, StringTable& strings) {
  auto count = decoder.Read<Int32>();
  for (Int32 i = 0; i < count; ++i)
    strings.Append(decoder.Read<String>());
}

NodeLoader::NodeLoader(NodeLoaderContext&& context)
    : NodeLoaderContext{std::move(context)} {}

void NodeLoader::LoadNodes() {
  StringTable namespace_uris;
  namespace_uris.Append("http://opcfoundation.org/UA/");
  LoadStringTable(decoder_, namespace_uris);

  StringTable server_uris;
  if (namespace_uris.GetCount() >= 2)
    server_uris.Append(namespace_uris.FindString(1));
  LoadStringTable(decoder_, server_uris);

  decoder_.set_namespace_mapping(
      MakeNamespaceMapping(namespace_uris, namespace_uris_));

  auto count = decoder_.Read<int32_t>();
  if (count <= 0)
    return;

  nodes_.reserve(nodes_.size() + count);
  for (int32_t i = 0; i < count; ++i)
    nodes_.emplace_back(LoadNode());
}

bool NodeLoader::HasAttribute(unsigned& attribute_mask,
                              AttributesToSave attribute_id) {
  if (!(attribute_mask & static_cast<unsigned>(attribute_id)))
    return false;
  attribute_mask &= ~static_cast<unsigned>(attribute_id);
  return true;
}

NodeState NodeLoader::LoadNode() {
  auto attribute_mask = decoder_.Read<uint32_t>();

  if (!HasAttribute(attribute_mask, AttributesToSave::NodeClass))
    throw std::runtime_error("No node class attribute");
  auto node_class = decoder_.ReadEnum<NodeClass>();

  String symbolic_name;
  if (HasAttribute(attribute_mask, AttributesToSave::SymbolicName))
    symbolic_name = decoder_.Read<String>();

  QualifiedName browse_name;
  if (HasAttribute(attribute_mask, AttributesToSave::BrowseName))
    browse_name = decoder_.Read<QualifiedName>();

  if (symbolic_name.empty())
    symbolic_name = browse_name.name();

  return LoadUnknownNode(attribute_mask, node_class, std::move(symbolic_name),
                         std::move(browse_name));
}

NodeState NodeLoader::LoadUnknownNode(unsigned& attribute_mask,
                                      NodeClass node_class,
                                      String&& symbolic_name,
                                      QualifiedName&& browse_name) {
  switch (node_class) {
    case OpcUa_NodeClass_Variable:
    case OpcUa_NodeClass_Object:
    case OpcUa_NodeClass_Method:
      return LoadUnknownChild(attribute_mask, node_class,
                              std::move(symbolic_name), std::move(browse_name));
  }

  NodeState node;
  node.node_class = node_class;
  if (!browse_name.empty())
    node.browse_name = std::move(browse_name);

  LoadAttributes(attribute_mask, node);
  LoadNodeReferences(node);
  LoadNodeChildren(node);

  return node;
}

NodeState NodeLoader::LoadUnknownChild(unsigned& attribute_mask,
                                       NodeClass node_class,
                                       String&& symbolic_name,
                                       QualifiedName&& browse_name) {
  NodeState node;
  node.node_class = node_class;
  node.browse_name = std::move(browse_name);

  if (HasAttribute(attribute_mask, AttributesToSave::NodeId))
    node.node_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::DisplayName))
    node.display_name = decoder_.Read<LocalizedText>();
  else if (!node.browse_name.empty())
    node.display_name = node.browse_name.name();

  if (HasAttribute(attribute_mask, AttributesToSave::Description))
    decoder_.Read<LocalizedText>();

  if (HasAttribute(attribute_mask, AttributesToSave::WriteMask))
    decoder_.Read<UInt32>();

  if (HasAttribute(attribute_mask, AttributesToSave::UserWriteMask))
    decoder_.Read<UInt32>();

  if (HasAttribute(attribute_mask, AttributesToSave::ReferenceTypeId))
    node.reference_type_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::TypeDefinitionId))
    node.type_definition_id = decoder_.Read<NodeId>();

  LoadAttributes(attribute_mask, node);
  LoadNodeReferences(node);
  LoadNodeChildren(node);

  return node;
}

void NodeLoader::LoadAttributes(unsigned& attribute_mask, NodeState& node) {
  switch (node.node_class) {
    case OpcUa_NodeClass_ReferenceType:
      LoadReferenceTypeAttributes(attribute_mask, node);
      break;
    case OpcUa_NodeClass_DataType:
      LoadTypeAttributes(attribute_mask, node);
      break;
    case OpcUa_NodeClass_ObjectType:
      LoadTypeAttributes(attribute_mask, node);
      break;
    case OpcUa_NodeClass_VariableType:
      LoadVariableTypeAttributes(attribute_mask, node);
      break;
    case OpcUa_NodeClass_Object:
      LoadObjectAttributes(attribute_mask, node);
      break;
    case OpcUa_NodeClass_Variable:
      LoadVariableAttributes(attribute_mask, node);
      break;
    case OpcUa_NodeClass_Method:
      LoadMethodAttributes(attribute_mask, node);
      break;
    default:
      assert(false);
      break;
  }
}

void NodeLoader::LoadBaseAttributes(unsigned& attribute_mask, NodeState& node) {
  if (HasAttribute(attribute_mask, AttributesToSave::NodeClass))
    node.node_class = decoder_.ReadEnum<NodeClass>();

  if (HasAttribute(attribute_mask, AttributesToSave::SymbolicName))
    decoder_.Read<String>();

  if (HasAttribute(attribute_mask, AttributesToSave::BrowseName))
    node.browse_name = decoder_.Read<QualifiedName>();

  if (HasAttribute(attribute_mask, AttributesToSave::NodeId))
    node.node_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::DisplayName))
    node.display_name = decoder_.Read<LocalizedText>();
  else if (!node.browse_name.empty())
    node.display_name = node.browse_name.name();

  if (HasAttribute(attribute_mask, AttributesToSave::Description))
    decoder_.Read<LocalizedText>();

  if (HasAttribute(attribute_mask, AttributesToSave::WriteMask))
    decoder_.Read<UInt32>();

  if (HasAttribute(attribute_mask, AttributesToSave::UserWriteMask))
    decoder_.Read<UInt32>();
}

void NodeLoader::LoadTypeAttributes(unsigned& attribute_mask, NodeState& node) {
  LoadBaseAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::SuperTypeId))
    node.super_type_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::IsAbstract))
    decoder_.Read<Boolean>();
}

void NodeLoader::LoadReferenceTypeAttributes(unsigned& attribute_mask,
                                             NodeState& node) {
  LoadTypeAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::InverseName))
    decoder_.Read<LocalizedText>();

  if (HasAttribute(attribute_mask, AttributesToSave::Symmetric))
    decoder_.Read<Boolean>();
}

void NodeLoader::LoadVariableTypeAttributes(unsigned& attribute_mask,
                                            NodeState& node) {
  LoadTypeAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::Value)) {
    node.value = decoder_.Read<Variant>();
    assert(!node.value.is_null());
  }

  if (HasAttribute(attribute_mask, AttributesToSave::DataType))
    node.data_type_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::ValueRank))
    decoder_.Read<Int32>();

  if (HasAttribute(attribute_mask, AttributesToSave::ArrayDimensions))
    decoder_.ReadArray<UInt32>();
}

void NodeLoader::LoadVariableAttributes(unsigned& attribute_mask,
                                        NodeState& node) {
  LoadInstanceAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::Value)) {
    node.value = decoder_.Read<Variant>();
    assert(!node.value.is_null());
  }

  if (HasAttribute(attribute_mask, AttributesToSave::StatusCode))
    decoder_.Read<StatusCode>();

  if (HasAttribute(attribute_mask, AttributesToSave::DataType))
    node.data_type_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::ValueRank))
    decoder_.Read<Int32>();

  if (HasAttribute(attribute_mask, AttributesToSave::ArrayDimensions))
    decoder_.ReadArray<UInt32>();

  if (HasAttribute(attribute_mask, AttributesToSave::AccessLevel))
    decoder_.Read<SByte>();

  if (HasAttribute(attribute_mask, AttributesToSave::UserAccessLevel))
    decoder_.Read<SByte>();

  if (HasAttribute(attribute_mask, AttributesToSave::MinimumSamplingInterval))
    decoder_.Read<Double>();

  if (HasAttribute(attribute_mask, AttributesToSave::Historizing))
    decoder_.Read<Boolean>();
}

void NodeLoader::LoadMethodAttributes(unsigned& attribute_mask,
                                      NodeState& node) {
  LoadInstanceAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::Executable))
    decoder_.Read<Boolean>();

  if (HasAttribute(attribute_mask, AttributesToSave::UserExecutable))
    decoder_.Read<Boolean>();
}

void NodeLoader::LoadInstanceAttributes(unsigned& attribute_mask,
                                        NodeState& node) {
  LoadBaseAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::ReferenceTypeId))
    node.reference_type_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::TypeDefinitionId))
    node.type_definition_id = decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::ModellingRuleId))
    decoder_.Read<NodeId>();

  if (HasAttribute(attribute_mask, AttributesToSave::NumericId))
    decoder_.Read<UInt32>();
}

void NodeLoader::LoadObjectAttributes(unsigned& attribute_mask,
                                      NodeState& node) {
  LoadInstanceAttributes(attribute_mask, node);

  if (HasAttribute(attribute_mask, AttributesToSave::EventNotifier))
    decoder_.Read<SByte>();
}

void NodeLoader::LoadNodeReferences(NodeState& node) {
  const auto count = decoder_.Read<int32_t>();
  if (count <= 0)
    return;

  node.references.reserve(node.references.size() + count);
  for (int i = 0; i < count; ++i) {
    auto reference_type_id = decoder_.Read<NodeId>();
    auto inverse = decoder_.Read<Boolean>();
    auto target_id = decoder_.Read<ExpandedNodeId>();
    node.references.push_back(
        {std::move(reference_type_id), inverse, std::move(target_id)});
  }
}

void NodeLoader::LoadNodeChildren(NodeState& parent) {
  const auto count = decoder_.Read<int32_t>();
  if (count <= 0)
    return;

  parent.children.reserve(nodes_.size() + count);
  for (int i = 0; i < count; ++i)
    parent.children.emplace_back(LoadChild());
}

NodeState NodeLoader::LoadChild() {
  auto attribute_mask = decoder_.Read<uint32_t>();

  if (!HasAttribute(attribute_mask, AttributesToSave::NodeClass))
    throw std::runtime_error("No node class attribute");
  auto node_class = decoder_.ReadEnum<NodeClass>();

  String symbolic_name;
  if (HasAttribute(attribute_mask, AttributesToSave::SymbolicName))
    symbolic_name = decoder_.Read<String>();

  QualifiedName browse_name;
  if (HasAttribute(attribute_mask, AttributesToSave::BrowseName))
    browse_name = decoder_.Read<QualifiedName>();

  if (symbolic_name.empty())
    symbolic_name = browse_name.name();

  return LoadUnknownChild(attribute_mask, node_class, std::move(symbolic_name),
                          std::move(browse_name));
}

std::vector<NodeState> LoadPredefinedNodes(const StringTable& namespace_uris,
                                           std::istream& stream) {
  EncodableTypeTable types;
  types.AddKnownTypes();

  MessageContext context;
  context.KnownTypes = &types.get();

  BinaryDecoder decoder;
  StdInputStream input{stream};
  decoder.Open(input.get(), context);

  std::vector<NodeState> nodes;

  NodeLoader loader{NodeLoaderContext{
      decoder,
      nodes,
      namespace_uris,
  }};
  loader.LoadNodes();

  return nodes;
}

std::vector<NodeState> LoadPredefinedNodes(const StringTable& namespace_uris,
                                           std::istream& stream);

}  // namespace server
}  // namespace opcua
