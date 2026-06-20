#include "opcua/client_monitored_item.h"

#include "opcua/client_subscription.h"

#include <utility>

namespace opcua {

MonitoredItem::MonitoredItem(
    std::shared_ptr<ClientSubscription> subscription,
    std::uint32_t local_id,
    scada::ReadValueId read_value_id,
    scada::MonitoringParameters params)
    : subscription_{std::move(subscription)},
      local_id_{local_id},
      read_value_id_{std::move(read_value_id)},
      params_{std::move(params)} {}

MonitoredItem::~MonitoredItem() {
  if (subscription_) {
    subscription_->Unsubscribe(local_id_);
  }
}

void MonitoredItem::Subscribe(scada::MonitoredItemHandler handler) {
  subscription_->Subscribe(local_id_, read_value_id_, params_,
                           std::move(handler));
}

}  // namespace opcua
