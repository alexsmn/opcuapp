#pragma once

#include <functional>
#include <map>
#include <memory>
#include <opcuapp/requests.h>

namespace opcua {
namespace server {

struct MonitoredItem {
};

struct SubscriptionContext {
  const opcua::SubscriptionId id_;
};

class Subscription : private SubscriptionContext {
 public:
  explicit Subscription(SubscriptionContext&& context) : SubscriptionContext{std::move(context)} {}

  SubscriptionId id() const { return id_; }

  void BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request, const std::function<void(OpcUa_CreateMonitoredItemsResponse& response)>& callback);
  void BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request, const std::function<void(OpcUa_DeleteMonitoredItemsResponse& response)>& callback);

 private:
  MonitoredItemCreateResult CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request);
  StatusCode DeleteMonitoredItem(opcua::MonitoredItemId monitored_item_id);

  void OnDataChange(MonitoredItemId monitored_item_id, const DataValue& data_value);

  struct ItemData {
    std::unique_ptr<MonitoredItem> item;
  };

  opcua::MonitoredItemId next_item_id_ = 1;
  std::map<MonitoredItemId, ItemData> items_;
};

} // namespace server
} // namespace opcua