#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/services/operation_limits.h"
#include "opcua/services/service_callbacks.h"
#include "opcua/services/service_context.h"
#include "opcua/services/service_message.h"

#include <memory>

namespace opcua {

struct ServiceHandlerContext {
  ServiceCallbacks callbacks;
  // The activated session's ServiceContext (user_id + user_rights + trace), so
  // every dispatched service call carries the caller's rights for the server's
  // authorization checks — not just the user id.
  ServiceContext service_context;
  OperationLimits operation_limits;
};

class ServiceHandler : private ServiceHandlerContext {
 public:
  explicit ServiceHandler(ServiceHandlerContext&& context);

  [[nodiscard]] Awaitable<ServiceResponse> Handle(ServiceRequest request) const;

 private:
  [[nodiscard]] Awaitable<ServiceResponse> HandleRead(
      ua::ReadRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleWrite(
      ua::WriteRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleBrowse(
      ua::BrowseRequest request) const;
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
      ua::AddNodesRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleDeleteNodes(
      ua::DeleteNodesRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleAddReferences(
      ua::AddReferencesRequest request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleDeleteReferences(
      ua::DeleteReferencesRequest request) const;
};

}  // namespace opcua
