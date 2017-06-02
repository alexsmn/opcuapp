#include <iostream>
#include <thread>

#include "opcuapp/proxy_stub.h"
#include "opcuapp/client/session.h"

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

class Client {
 private:
  opcua::Platform platform;
  opcua::ProxyStub proxy_stub_{platform, MakeProxyStubConfiguration()};
  opcua::Channel channel_{OpcUa_Channel_SerializerType_Binary};
  opcua::client::Session session_{channel_};
};

int main() {
  try {
    std::cout << "Starting..." << std::endl;
    Client client;

    std::cout << "Running..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "Terminating..." << std::endl;

  } catch (const std::exception& e) {
    std::cout << "ERROR: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
