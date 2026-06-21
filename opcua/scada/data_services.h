#pragma once

#include "opcua/scada/services.h"

#include <memory>

namespace opcua {
namespace scada {
class HistoryService;
class MethodService;
class NodeManagementService;
class ViewService;
class AttributeService;
}  // namespace scada

// TODO: Move under `scada` namespace.
// TODO: Replace with `std::shared_ptr<opcua::scada::services>`.
struct DataServices {
  opcua::scada::services as_services() const {
    return {.attribute_service = attribute_service_.get(),
            .monitored_item_service = monitored_item_service_.get(),
            .method_service = method_service_.get(),
            .history_service = history_service_.get(),
            .view_service = view_service_.get(),
            .node_management_service = node_management_service_.get(),
            .session_service = session_service_.get()};
  }

  // The `services` are owned by the caller and must outlast the returned
  // instance.
  static DataServices FromUnownedServices(const opcua::scada::services& services) {
    // Create a fake pointer.
    return FromSharedServices(std::make_shared<opcua::scada::services>(services));
  }

  static DataServices FromSharedServices(
      const std::shared_ptr<const opcua::scada::services>& shared_services) {
    return {
        .session_service_ =
            std::shared_ptr<opcua::scada::SessionService>{
                shared_services, shared_services->session_service},
        .view_service_ =
            std::shared_ptr<opcua::scada::ViewService>{shared_services,
                                                shared_services->view_service},
        .node_management_service_ =
            std::shared_ptr<opcua::scada::NodeManagementService>{
                shared_services, shared_services->node_management_service},
        .history_service_ =
            std::shared_ptr<opcua::scada::HistoryService>{
                shared_services, shared_services->history_service},
        .attribute_service_ =
            std::shared_ptr<opcua::scada::AttributeService>{
                shared_services, shared_services->attribute_service},
        .method_service_ =
            std::shared_ptr<opcua::scada::MethodService>{
                shared_services, shared_services->method_service},
        .monitored_item_service_ = std::shared_ptr<opcua::scada::MonitoredItemService>{
            shared_services, shared_services->monitored_item_service}};
  }

  std::shared_ptr<opcua::scada::SessionService> session_service_;
  std::shared_ptr<opcua::scada::ViewService> view_service_;
  std::shared_ptr<opcua::scada::NodeManagementService> node_management_service_;
  std::shared_ptr<opcua::scada::HistoryService> history_service_;
  std::shared_ptr<opcua::scada::AttributeService> attribute_service_;
  std::shared_ptr<opcua::scada::MethodService> method_service_;
  std::shared_ptr<opcua::scada::MonitoredItemService> monitored_item_service_;
};
}  // namespace opcua (vendored)
