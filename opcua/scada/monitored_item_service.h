#pragma once

#include "opcua/scada/monitored_item.h"
#include "opcua/scada/service_context.h"
#include "opcua/scada/status_or.h"

#include <memory>

namespace opcua {
namespace scada {

// opcuapp service interface for creating MonitoredItem subscriptions; the
// implementation backing the OPC UA MonitoredItem Service Set. OPC UA Part 4
// §5.13 MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
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

}  // namespace scada
}  // namespace opcua (vendored)
