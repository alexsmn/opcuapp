#pragma once

#include "opcua/scada/monitored_item.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/read_value_id.h"

#include <cstdint>
#include <memory>

namespace opcua {

class ClientSubscription;

// scada::MonitoredItem instance returned to the Qt client from
// ClientSession::CreateMonitoredItem. Subscribe() binds a handler and
// launches the server-side monitored item through the owning subscription;
// the destructor unsubscribes.
class MonitoredItem final : public scada::MonitoredItem {
 public:
  MonitoredItem(std::shared_ptr<ClientSubscription> subscription,
                     std::uint32_t local_id,
                     ReadValueId read_value_id,
                     scada::MonitoringParameters params);
  ~MonitoredItem() override;

  void Subscribe(scada::MonitoredItemHandler handler) override;

 private:
  const std::shared_ptr<ClientSubscription> subscription_;
  const std::uint32_t local_id_;
  const ReadValueId read_value_id_;
  const scada::MonitoringParameters params_;
};

}  // namespace opcua
