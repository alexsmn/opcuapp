#include <fstream>
#include <gtest/gtest.h>
#include <map>

#include "opcua/server/node_loader.h"
#include "opcua/types.h"
#include "scada/core/standard_node_ids.h"

namespace opcua {
namespace server {

OpcUa_ProxyStubConfiguration MakeProxyStubConfiguration() {
  OpcUa_ProxyStubConfiguration result = {};
  result.bProxyStub_Trace_Enabled = OpcUa_True;   //to deactivate Tracer set this variable Opc Ua False.
  result.uProxyStub_Trace_Level = OPCUA_TRACE_OUTPUT_LEVEL_WARNING;
  result.iSerializer_MaxAlloc = -1;
  result.iSerializer_MaxStringLength = -1;
  result.iSerializer_MaxByteStringLength = -1;
  result.iSerializer_MaxArrayLength = -1;
  result.iSerializer_MaxMessageSize = -1;
  result.iSerializer_MaxRecursionDepth = -1;
  result.bSecureListener_ThreadPool_Enabled = OpcUa_False;
  result.iSecureListener_ThreadPool_MinThreads = -1;
  result.iSecureListener_ThreadPool_MaxThreads = -1;
  result.iSecureListener_ThreadPool_MaxJobs = -1;
  result.bSecureListener_ThreadPool_BlockOnAdd = OpcUa_True;
  result.uSecureListener_ThreadPool_Timeout = OPCUA_INFINITE;
  result.bTcpListener_ClientThreadsEnabled = OpcUa_False;
  result.iTcpListener_DefaultChunkSize = -1;
  result.iTcpConnection_DefaultChunkSize = -1;
  result.iTcpTransport_MaxMessageLength = -1;
  result.iTcpTransport_MaxChunkCount = -1;
  result.bTcpStream_ExpectWriteToBlock = OpcUa_True;
  return result;
}

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

TEST(NodeLoader, Test) {
  const Platform platform;
  const ProxyStub proxy_stub_{platform, MakeProxyStubConfiguration()};

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
}

} // namespace server
} // namespace opcua
