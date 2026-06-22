#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/services/operation_limits.h"
#include "opcua/services/service_callbacks.h"
#include "opcua/services/service_message.h"

#include <memory>

namespace opcua {

struct ServiceHandlerContext {
  ServiceCallbacks callbacks;
  NodeId user_id;
  OperationLimits operation_limits;
};

class ServiceHandler : private ServiceHandlerContext {
 public:
  explicit ServiceHandler(ServiceHandlerContext&& context);

  [[nodiscard]] Awaitable<ServiceResponse> Handle(ServiceRequest request) const;

 private:
  [[nodiscard]] Awaitable<ServiceResponse> HandleRead(
      ReadRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleWrite(
      WriteRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleBrowse(
      BrowseRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleTranslateBrowsePaths(
      TranslateBrowsePathsRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleCall(
      CallRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleHistoryReadRaw(
      HistoryReadRawRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleHistoryReadEvents(
      HistoryReadEventsRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleHistoryUpdate(
      HistoryUpdateRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleAddNodes(
      AddNodesRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleDeleteNodes(
      DeleteNodesRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleAddReferences(
      AddReferencesRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleDeleteReferences(
      DeleteReferencesRequest request) const;
};

}  // namespace opcua
