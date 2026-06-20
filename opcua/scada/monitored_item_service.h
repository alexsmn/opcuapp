#pragma once

#include "opcua/scada/monitored_item.h"
#include "opcua/scada/service_context.h"
#include "opcua/scada/status_or.h"

#include <memory>

namespace opcua::scada {

class MonitoredItemService {
 public:
  virtual ~MonitoredItemService() {}

  // Creates a subscription stream that adds and removes monitored items and
  // delivers their data-change and event notifications as coroutine-read
  // batches. Implementations that retain single-item creation logic can build a
  // subscription with `MakeItemFactorySubscription`
  // (item_factory_subscription.h) and expose single items to callers through
  // `LegacyMonitoredItemAdapter`.
  virtual StatusOr<std::unique_ptr<MonitoredItemSubscription>>
  CreateSubscription(ServiceContext context,
                     MonitoredItemSubscriptionOptions options) = 0;
};

}  // namespace opcua::scada
