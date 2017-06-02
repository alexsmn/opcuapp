#pragma once

#include "opcuapp/types.h"

namespace opcua {

class ProxyStub {
 public:
  ProxyStub(const Platform& platform, const OpcUa_ProxyStubConfiguration& configuration);
  ProxyStub(const Platform& platform, const OpcUa_ProxyStubConfiguration& configuration, StatusCode& status_code);
  ~ProxyStub();

 private:
  StatusCode status_code_ = OpcUa_Bad;
};

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
