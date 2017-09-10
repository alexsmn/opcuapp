#pragma once

#include <functional>
#include <map>
#include <memory>
#include <opcuapp/extension_object.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/handlers.h>
#include <queue>
#include <vector>

namespace opcua {
namespace server {

struct SubscriptionContext {
  const opcua::SubscriptionId id_;
  const CreateMonitoredItemHandler create_monitored_item_handler_;
};

class Subscription : private SubscriptionContext {
 public:
  explicit Subscription(SubscriptionContext&& context) : SubscriptionContext{std::move(context)} {}

  SubscriptionId id() const { return id_; }

  void BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request, const std::function<void(OpcUa_CreateMonitoredItemsResponse& response)>& callback);
  void BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request, const std::function<void(OpcUa_DeleteMonitoredItemsResponse& response)>& callback);

  void Acknowledge(UInt32 sequence_number);
  bool Publish(PublishResponse& response);

 private:
  MonitoredItemCreateResult CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request);
  StatusCode DeleteMonitoredItem(opcua::MonitoredItemId monitored_item_id);

  void OnDataChange(MonitoredItemClientHandle client_handle, DataValue&& data_value);

  struct ItemData {
    MonitoredItemClientHandle client_handle;
    std::shared_ptr<MonitoredItem> item;
  };

  std::vector<ExtensionObject> notifications_;

  UInt32 next_sequence_number_ = 0;
  std::map<UInt32 /*sequence_number*/, NotificationMessage> published_notifications_;

  opcua::MonitoredItemId next_item_id_ = 1;
  std::map<MonitoredItemId, ItemData> items_;
};

} // namespace server
} // namespace opcua