#pragma once

#include "opcua/base/any_executor.h"

namespace opcua {

class AttributeService;
class MethodService;
class NodeManagementService;
class HistoryService;
class SessionService;
class ViewService;
// MonitoredItemService is still nested (its monitored-item structs are reworked
// to wire forms in a later stage).
namespace scada {
class MonitoredItemService;
}  // namespace scada

// TODO: Sort services by names.
struct services {
  AttributeService* attribute_service = nullptr;
  scada::MonitoredItemService* monitored_item_service = nullptr;
  MethodService* method_service = nullptr;
  HistoryService* history_service = nullptr;
  ViewService* view_service = nullptr;
  NodeManagementService* node_management_service = nullptr;
  SessionService* session_service = nullptr;

  // Executor used by the client-side `monitored_item` helper to drive the
  // subscription that backs single monitored items (via
  // `LegacyMonitoredItemAdapter`). Only required on `services` that flow into
  // the client `monitored_item::subscribe` path.
  AnyExecutor monitored_item_executor = {};
};

}  // namespace opcua (vendored)
