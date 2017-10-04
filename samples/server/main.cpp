#include <fstream>
#include <iostream>
#include <opcuapp/extension_object.h>
#include <opcuapp/platform.h>
#include <opcuapp/proxy_stub.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/endpoint.h>
#include <opcuapp/server/node_loader.h>
#include <opcuapp/vector.h>
#include <opcuapp/timer.h>
#include <thread>

const char kPredefinedNodesPath[] = "Opc.Ua.PredefinedNodes.uanodes";

namespace {

class Variable : public opcua::server::MonitoredItem {
 public:
  using ReadHandler = std::function<opcua::DataValue(opcua::AttributeId attribute_id)>;

  explicit Variable(ReadHandler handler) : read_handler_{std::move(handler)} {}

  opcua::DataValue Read(opcua::AttributeId attribute_id) const;

  // opcua::server::MonitoredItem
  virtual void Subscribe(const opcua::server::DataChangeHandler& data_change_handler) override;

 private:
  const ReadHandler read_handler_;

  opcua::Timer timer_;
};

opcua::DataValue Variable::Read(opcua::AttributeId attribute_id) const {
  return read_handler_(attribute_id);
}

void Variable::Subscribe(const opcua::server::DataChangeHandler& data_change_handler) {
  {
    auto data_value = Read(OpcUa_Attributes_Value);
    if (data_value.status_code())
      data_change_handler(std::move(data_value));
  }

  timer_.set_interval(1000);

  // TODO: Update rate.
  timer_.Start([this, data_change_handler] {
    auto data_value = Read(OpcUa_Attributes_Value);
    if (data_value.status_code())
      data_change_handler(std::move(data_value));
  });
}

} // namespace

class Server {
 public:
  Server();

 private:
  std::shared_ptr<Variable> GetVariable(const OpcUa_NodeId& node_id) const;
  const opcua::server::NodeState* GetStaticNode(const OpcUa_NodeId& node_id) const;

  opcua::DataValue Read(const OpcUa_ReadValueId& read_value_id) const;

  opcua::Platform platform_;
  opcua::ProxyStub proxy_stub_{platform_, opcua::ProxyStubConfiguration{}};

  opcua::StringTable namespace_uris_;

  const opcua::ByteString server_certificate_;
  const opcua::server::Endpoint::SecurityPolicyConfiguration security_policy_;
  const OpcUa_Key server_private_key{OpcUa_Crypto_KeyType_Invalid, {0, (OpcUa_Byte*)""}};
  const OpcUa_P_OpenSSL_CertificateStore_Config pki_config_{OpcUa_NO_PKI, OpcUa_Null, OpcUa_Null,OpcUa_Null, 0, OpcUa_Null};
  opcua::server::Endpoint endpoint_{OpcUa_Endpoint_SerializerType_Binary};

  const opcua::DateTime start_time_ = opcua::DateTime::UtcNow();

  std::map<opcua::NodeId, opcua::server::NodeState> static_nodes_;
  std::map<opcua::NodeId, std::shared_ptr<Variable>> variables_;
};

Server::Server() {
  std::ifstream stream{kPredefinedNodesPath, std::ios::in | std::ios::binary};
  if (!stream)
    throw std::runtime_error{std::string{"Can't open file "} + kPredefinedNodesPath};

  for (auto& static_node : opcua::server::LoadPredefinedNodes(namespace_uris_, stream)) {
    opcua::NodeId node_id = static_node.node_id;
    static_nodes_.emplace(std::move(node_id), std::move(static_node));
  }

  variables_.emplace(OpcUaId_Server_ServerStatus,
      std::make_shared<Variable>([this](opcua::AttributeId attribute_id) -> opcua::DataValue {
        if (attribute_id != OpcUa_Attributes_Value)
          return OpcUa_Bad;
        std::cout << "Read server status" << std::endl;
        const auto time = opcua::DateTime::UtcNow();
        opcua::ServerStatusDataType server_status;
        server_status.CurrentTime = time.get();
        server_status.State = OpcUa_ServerState_Running;
        server_status.StartTime = start_time_.get();
        return {OpcUa_Good, opcua::ExtensionObject::Encode(std::move(server_status)), time, time};
      }));

  variables_.emplace(OpcUaId_Server_ServerStatus_CurrentTime,
      std::make_shared<Variable>([](opcua::AttributeId attribute_id) -> opcua::DataValue {
        if (attribute_id != OpcUa_Attributes_Value)
          return OpcUa_Bad;
        std::cout << "Read current time" << std::endl;
        const auto time = opcua::DateTime::UtcNow();
        return {OpcUa_Good, time, time, time};
      }));

  endpoint_.set_status_handler([](opcua::server::Endpoint::Event event) {
    switch (event) {
      case eOpcUa_Endpoint_Event_SecureChannelOpened:
        std::cout << "Endpoint SecureChannelOpened" << std::endl;
        break;
      case eOpcUa_Endpoint_Event_SecureChannelClosed:
        std::cout << "Endpoint SecureChannelClosed" << std::endl;
        break;
      case eOpcUa_Endpoint_Event_SecureChannelRenewed:
        std::cout << "Endpoint SecureChannelRenewed" << std::endl;
        break;
      case eOpcUa_Endpoint_Event_UnsupportedServiceRequested:
        std::cout << "Endpoint UnsupportedServiceRequested" << std::endl;
        break;
      case eOpcUa_Endpoint_Event_DecoderError:
        std::cout << "Endpoint DecoderError" << std::endl;
        break;
      default:
        std::cout << "Endpoint InvalidEvent" << std::endl;
        break;
    }
  });

  endpoint_.set_read_handler([this](OpcUa_ReadRequest& request, const opcua::server::ReadCallback& callback) {
    opcua::Span<OpcUa_ReadValueId> read_value_ids{request.NodesToRead, static_cast<size_t>(request.NoOfNodesToRead)};
    std::cout << "Read " << read_value_ids.size() << " values" << std::endl;

    opcua::Vector<OpcUa_DataValue> results(read_value_ids.size());
    for (size_t i = 0; i < read_value_ids.size(); ++i)
      Read(read_value_ids[i]).release(results[i]);

    opcua::ReadResponse response;
    response.ResponseHeader.ServiceResult = OpcUa_Good;
    response.NoOfResults = results.size();
    response.Results = results.release();
    callback(response);
  });

  endpoint_.set_browse_handler([](OpcUa_BrowseRequest& request, const opcua::server::BrowseCallback& callback) {
    std::cout << "Browse" << std::endl;
    opcua::BrowseResponse response;
    response.ResponseHeader.ServiceResult = OpcUa_Bad;
    callback(response);
  });

  endpoint_.set_create_monitored_item_handler(
      [this](opcua::ReadValueId& read_value_id) -> opcua::server::CreateMonitoredItemResult {
        std::cout << "CreateMonitoredItem" << std::endl;
        auto variable = GetVariable(read_value_id.NodeId);
        if (!variable)
          return {OpcUa_Bad};
        return {OpcUa_Good, variable};
      });

  opcua::String url = "opc.tcp://localhost:4840";
  endpoint_.Open(std::move(url), true, server_certificate_.get(), server_private_key, &pki_config_,
      {&security_policy_, 1});
}

std::shared_ptr<Variable> Server::GetVariable(const OpcUa_NodeId& node_id) const {
  auto i = variables_.find(node_id);
  return i != variables_.end() ? i->second : nullptr;
}

const opcua::server::NodeState* Server::GetStaticNode(const OpcUa_NodeId& node_id) const {
  auto i = static_nodes_.find(node_id);
  return i != static_nodes_.end() ? &i->second : nullptr;
}

opcua::DataValue Server::Read(const OpcUa_ReadValueId& read_value_id) const {
  if (auto variable = GetVariable(read_value_id.NodeId)) {
    return variable->Read(read_value_id.AttributeId);

  } else if (auto* static_node = GetStaticNode(read_value_id.NodeId)) {
    if (read_value_id.AttributeId != OpcUa_Attributes_Value)
      return OpcUa_Bad;
    auto timestamp = opcua::DateTime::UtcNow();
    return {OpcUa_Good, static_node->value, timestamp, timestamp};

  } else {
    return OpcUa_Bad;
  }
}

int main() {
  try {
    std::cout << "Starting..." << std::endl;
    Server server;

    std::cout << "Running..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(500));

    std::cout << "Terminating..." << std::endl;

  } catch (const std::exception& e) {
    std::cout << "ERROR: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
