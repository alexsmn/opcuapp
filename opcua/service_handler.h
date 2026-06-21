#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/operation_limits.h"
#include "opcua/service_message.h"

#include "opcua/scada/attribute_service.h"
#include "opcua/scada/history_service.h"
#include "opcua/scada/method_service.h"
#include "opcua/scada/node_management_service.h"
#include "opcua/scada/view_service.h"

#include <memory>

namespace opcua {
class AttributeService;
class HistoryService;
class MethodService;
class NodeManagementService;
class ViewService;
}  // namespace opcua

namespace opcua {

struct ServiceHandlerContext {
  AttributeService& attribute_service;
  ViewService& view_service;
  HistoryService& history_service;
  MethodService& method_service;
  NodeManagementService& node_management_service;
  NodeId user_id;
  OperationLimits operation_limits;
};

class ServiceHandler : private ServiceHandlerContext {
 public:
  explicit ServiceHandler(ServiceHandlerContext&& context);

  [[nodiscard]] Awaitable<ServiceResponse> Handle(
      ServiceRequest request) const;

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
