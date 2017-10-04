#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <opcuapp/platform.h>
#include <opcuapp/proxy_stub.h>
#include <opcuapp/server/node_loader.h>
#include <opcuapp/basic_types.h>
#include <opcuapp/string_table.h>

namespace opcua {
namespace server {

class AddressSpace {
 public:
  void Load(const char* path) {
    StringTable namespace_uris;
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    nodes_ = LoadPredefinedNodes(namespace_uris, stream);
    IndexNodes(nodes_);
  }

  NodeState* GetNode(const NodeId& node_id) {
    auto i = node_map_.find(node_id);
    return i != node_map_.end() ? i->second : nullptr;
  }

 private:
  void IndexNodes(std::vector<NodeState>& nodes) {
    for (auto& node : nodes) {
      node_map_.emplace(node.node_id, &node);
      IndexNodes(node.children);
    }
  }

  std::vector<NodeState> nodes_;
  std::map<NodeId, NodeState*> node_map_;
};

/*TEST(NodeLoader, Test) {
  const Platform platform;
  const ProxyStub proxy_stub_{platform, ProxyStubConfiguration{}};

  AddressSpace address_space;
  address_space.Load("Opc.Ua.uanodes");

  {
    auto* node = address_space.GetNode(OpcUaId_Server_ServerStatus_State);
    ASSERT_NE(nullptr, node);
  }

  {
    auto* node = address_space.GetNode(OpcUaId_ServerState_EnumStrings);
    ASSERT_NE(nullptr, node);
    auto& values = node->value;
    EXPECT_EQ(OpcUaType_LocalizedText, values.data_type());
    EXPECT_TRUE(values.is_array());
    auto strings = values.get<Span<const OpcUa_LocalizedText>>();
    EXPECT_EQ(8, strings.size());
  }
}*/

} // namespace server
} // namespace opcua
