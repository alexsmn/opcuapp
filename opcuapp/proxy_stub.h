#pragma once

#include <opcuapp/platform.h>
#include <opcuapp/structs.h>

namespace opcua {

struct ProxyStubConfiguration : OpcUa_ProxyStubConfiguration {
  ProxyStubConfiguration();
};

class ProxyStub {
 public:
  ProxyStub(const Platform& platform, const OpcUa_ProxyStubConfiguration& configuration);
  ProxyStub(const Platform& platform, const OpcUa_ProxyStubConfiguration& configuration, StatusCode& status_code);
  ~ProxyStub();

 private:
  StatusCode status_code_ = OpcUa_Bad;
};

inline ProxyStubConfiguration::ProxyStubConfiguration() {
  bProxyStub_Trace_Enabled = OpcUa_True;
  uProxyStub_Trace_Level = OPCUA_TRACE_OUTPUT_LEVEL_WARNING;
  iSerializer_MaxAlloc = -1;
  iSerializer_MaxStringLength = -1;
  iSerializer_MaxByteStringLength = -1;
  iSerializer_MaxArrayLength = -1;
  iSerializer_MaxMessageSize = -1;
  iSerializer_MaxRecursionDepth = -1;
  bSecureListener_ThreadPool_Enabled = OpcUa_False;
  iSecureListener_ThreadPool_MinThreads = -1;
  iSecureListener_ThreadPool_MaxThreads = -1;
  iSecureListener_ThreadPool_MaxJobs = -1;
  bSecureListener_ThreadPool_BlockOnAdd = OpcUa_True;
  uSecureListener_ThreadPool_Timeout = OPCUA_INFINITE;
  bTcpListener_ClientThreadsEnabled = OpcUa_False;
  iTcpListener_DefaultChunkSize = -1;
  iTcpConnection_DefaultChunkSize = -1;
  iTcpTransport_MaxMessageLength = -1;
  iTcpTransport_MaxChunkCount = -1;
  bTcpStream_ExpectWriteToBlock = OpcUa_True;
}

inline ProxyStub::ProxyStub(const Platform& platform, const OpcUa_ProxyStubConfiguration& configuration) {
  status_code_ = ::OpcUa_ProxyStub_Initialize(platform.handle(), &const_cast<OpcUa_ProxyStubConfiguration&>(configuration));
  Check(status_code_);
}

inline ProxyStub::ProxyStub(const Platform& platform, const OpcUa_ProxyStubConfiguration& configuration, StatusCode& status_code) {
  status_code = status_code_ = ::OpcUa_ProxyStub_Initialize(platform.handle(), &const_cast<OpcUa_ProxyStubConfiguration&>(configuration));
}

inline ProxyStub::~ProxyStub() {
  if (status_code_)
    ::OpcUa_ProxyStub_Clear();
}

} // namespace opcua
