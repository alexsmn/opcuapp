#pragma once

#include <cassert>
#include <functional>
#include <opcua_clientproxy.h>
#include <opcua_core.h>

namespace opcua {
namespace client {

template<class Response>
class AsyncRequest {
 public:
  using Callback = std::function<void(Response& response)>;

  explicit AsyncRequest(Callback callback) : callback_{std::move(callback)} {}

  static OpcUa_StatusCode OnComplete(
      OpcUa_Channel         hChannel,
      OpcUa_Void*           pResponse,
      OpcUa_EncodeableType* pResponseType,
      OpcUa_Void*           pCallbackData,
      OpcUa_StatusCode      uStatus);

 private:
  Callback callback_;
};

// static
template<class Response>
inline OpcUa_StatusCode AsyncRequest<Response>::OnComplete(
    OpcUa_Channel         hChannel,
    OpcUa_Void*           pResponse,
    OpcUa_EncodeableType* pResponseType,
    OpcUa_Void*           pCallbackData,
    OpcUa_StatusCode      uStatus) {
  using Request = AsyncRequest<Response>;
  std::unique_ptr<Request> request{static_cast<Request*>(pCallbackData)};

  if (!OpcUa_IsGood(uStatus)) {
    Response response;
    response.ResponseHeader.ServiceResult = uStatus;
    request->callback_(response);

  } else if (pResponseType->TypeId == Response::type().TypeId) {
    auto& response = *reinterpret_cast<Response*>(pResponse);
    static_cast<AsyncRequest<Response>*>(pCallbackData)->callback_(response);

  } else if (pResponseType->TypeId == OpcUaId_ServiceFault) {
    auto& service_fault = *reinterpret_cast<OpcUa_ServiceFault*>(pResponse);
    Response response;
    response.ResponseHeader = service_fault.ResponseHeader;
    request->callback_(response);

  } else {
    assert(false);
    pResponseType->Clear(pResponse);
  }

  return OpcUa_Good;
}

} // namespace client
} // namespace opcua
