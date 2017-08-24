#include <fstream>
#include <iostream>
#include <opcuapp/platform.h>
#include <opcuapp/proxy_stub.h>
#include <opcuapp/server/endpoint.h>
#include <opcuapp/server/node_loader.h>
#include <opcuapp/requests.h>
#include <thread>

const char kPredefinedNodesPath[] = "Opc.Ua.PredefinedNodes.uanodes";

namespace {

OpcUa_ProxyStubConfiguration MakeProxyStubConfiguration() {
  OpcUa_ProxyStubConfiguration result = {};
  result.bProxyStub_Trace_Enabled = OpcUa_True;
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

} // namespace

class Server {
 public:
  Server();

 private:
  opcua::Platform platform_;
  opcua::ProxyStub proxy_stub_{platform_, MakeProxyStubConfiguration()};

  opcua::StringTable namespace_uris_;

  const opcua::ByteString server_certificate_;
  const opcua::server::Endpoint::SecurityPolicyConfiguration security_policy_;
  const OpcUa_Key server_private_key{OpcUa_Crypto_KeyType_Invalid, {0, (OpcUa_Byte*)""}};
  const OpcUa_P_OpenSSL_CertificateStore_Config pki_config_{OpcUa_NO_PKI, OpcUa_Null, OpcUa_Null,OpcUa_Null, 0, OpcUa_Null};
  opcua::server::Endpoint endpoint_{OpcUa_Endpoint_SerializerType_Binary};
};

Server::Server() {
  std::ifstream stream{kPredefinedNodesPath, std::ios::in | std::ios::binary};
  if (!stream)
    throw std::runtime_error{std::string{"Can't open file "} + kPredefinedNodesPath};

  auto nodes = opcua::server::LoadPredefinedNodes(namespace_uris_, stream);

  endpoint_.set_read_handler([](OpcUa_ReadRequest& request, const opcua::server::ReadCallback& callback) {
    opcua::ReadResponse response;
    response.ResponseHeader.ServiceResult = OpcUa_Bad;
    callback(response);
  });

  endpoint_.set_browse_handler([](OpcUa_BrowseRequest& request, const opcua::server::BrowseCallback& callback) {
    opcua::BrowseResponse response;
    response.ResponseHeader.ServiceResult = OpcUa_Bad;
    callback(response);
  });

  opcua::String url = "opc.tcp://localhost:4840";
  endpoint_.Open(std::move(url), true, server_certificate_.get(), server_private_key, &pki_config_,
      {&security_policy_, 1}, [] {});
}

int main() {
  try {
    std::cout << "Starting..." << std::endl;
    Server server;

    std::cout << "Running..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "Terminating..." << std::endl;

  } catch (const std::exception& e) {
    std::cout << "ERROR: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
