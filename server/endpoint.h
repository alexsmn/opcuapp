#pragma once

#include "opcua/status_code.h"
#include "opcua/types.h"

#include <functional>
#include <memory>
#include <opcua_endpoint.h>

namespace opcua {
namespace server {

class EndpointCallbackWrapper {
 public:
  using Callback = std::function<void()>;
  explicit EndpointCallbackWrapper(Callback callback);

  static OpcUa_StatusCode Invoke(
      OpcUa_Endpoint          hEndpoint,
      OpcUa_Void*             pvCallbackData,
      OpcUa_Endpoint_Event    eEvent,
      OpcUa_StatusCode        uStatus,
      OpcUa_UInt32            uSecureChannelId,
      OpcUa_ByteString*       pbsClientCertificate,
      OpcUa_String*           pSecurityPolicy,
      OpcUa_UInt16            uSecurityMode);

 private:
  const Callback callback_;
};

inline EndpointCallbackWrapper::EndpointCallbackWrapper(Callback callback)
    : callback_{std::move(callback)} {
}

// static
inline OpcUa_StatusCode EndpointCallbackWrapper::Invoke(
      OpcUa_Endpoint          hEndpoint,
      OpcUa_Void*             pvCallbackData,
      OpcUa_Endpoint_Event    eEvent,
      OpcUa_StatusCode        uStatus,
      OpcUa_UInt32            uSecureChannelId,
      OpcUa_ByteString*       pbsClientCertificate,
      OpcUa_String*           pSecurityPolicy,
      OpcUa_UInt16            uSecurityMode) {
  auto& wrapper = *static_cast<EndpointCallbackWrapper*>(pvCallbackData);
  wrapper.callback_();
  // TODO: Delete.
  return OpcUa_Good;
}

class Endpoint {
 public:
  Endpoint(OpcUa_Endpoint_SerializerType serializer_type, OpcUa_ServiceType const** supported_services);
  ~Endpoint();

  using Callback = std::function<void()>;
  using SecurityPolicyConfiguration = OpcUa_Endpoint_SecurityPolicyConfiguration;

  void Open(OpcUa_StringA                           sUrl,
            OpcUa_Boolean                           bListenOnAllInterfaces,
            Callback                                callback,
            OpcUa_ByteString*                       pServerCertificate,
            OpcUa_Key*                              pServerPrivateKey,
            OpcUa_Void*                             pPKIConfig,
            Span<const SecurityPolicyConfiguration> security_policies);

  OpcUa_Handle handle() const { return handle_; }

 private:
  OpcUa_Endpoint handle_ = OpcUa_Null;
};

inline Endpoint::Endpoint(OpcUa_Endpoint_SerializerType serializer_type, OpcUa_ServiceType const** supported_services) {
  Check(::OpcUa_Endpoint_Create(&handle_, serializer_type, const_cast<OpcUa_ServiceType**>(supported_services)));
}

inline Endpoint::~Endpoint() {
  ::OpcUa_Endpoint_Delete(&handle_);
}

inline void Endpoint::Open(OpcUa_StringA                           sUrl,
                           OpcUa_Boolean                           bListenOnAllInterfaces,
                           Callback                                callback,
                           OpcUa_ByteString*                       pServerCertificate,
                           OpcUa_Key*                              pServerPrivateKey,
                           OpcUa_Void*                             pPKIConfig,
                           Span<const SecurityPolicyConfiguration> security_policies) {
  auto callback_wrapper = std::make_unique<EndpointCallbackWrapper>(std::move(callback));
  Check(::OpcUa_Endpoint_Open(
      handle_, 
      sUrl,
      bListenOnAllInterfaces,
      &EndpointCallbackWrapper::Invoke,
      callback_wrapper.release(),
      pServerCertificate,
      pServerPrivateKey,
      pPKIConfig,
      security_policies.size(),
      const_cast<SecurityPolicyConfiguration*>(security_policies.data())));
}

} // namespace server
} // namespace opcua
