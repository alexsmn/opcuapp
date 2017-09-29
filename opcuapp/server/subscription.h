#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <opcuapp/extension_object.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/timer.h>
#include <queue>
#include <vector>

namespace opcua {
namespace server {

struct SubscriptionContext {
  const SubscriptionId id_;
  const CreateMonitoredItemHandler create_monitored_item_handler_;
  const std::function<void()> publish_handler_;
  const Double publishing_interval_ms_;
  const UInt32 lifetime_count_;
  const UInt32 max_keep_alive_count_;
  const size_t max_notifications_per_publish_;
  const bool publishing_enabled_;
  const Byte priority_;
};

class Subscription : private SubscriptionContext {
 public:
  explicit Subscription(SubscriptionContext&& context);

  SubscriptionId id() const { return id_; }

  void BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request, const std::function<void(OpcUa_CreateMonitoredItemsResponse& response)>& callback);
  void BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request, const std::function<void(OpcUa_DeleteMonitoredItemsResponse& response)>& callback);

  void Acknowledge(UInt32 sequence_number);
  bool Publish(PublishResponse& response);

 private:
  bool instant_publishing() const;

  MonitoredItemCreateResult CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request);
  StatusCode DeleteMonitoredItem(MonitoredItemId monitored_item_id);

  void OnDataChange(MonitoredItemClientHandle client_handle, DataValue&& data_value);
  void OnNotification(ExtensionObject&& notification);

  void OnPublishingTimer();

  bool PublishMessage(NotificationMessage& message);

  UInt32 MakeNextSequenceNumber();

  struct ItemData {
    MonitoredItemClientHandle client_handle;
    std::shared_ptr<MonitoredItem> item;
  };

  std::mutex mutex_;

  std::queue<ExtensionObject> notifications_;

  UInt32 keep_alive_count_ = 0;

  UInt32 next_sequence_number_ = 1;
  std::map<UInt32 /*sequence_number*/, NotificationMessage> published_messages_;

  MonitoredItemId next_item_id_ = 1;
  std::map<MonitoredItemId, ItemData> items_;

  Timer publishing_timer_;

  const Double kMinPublishingIntervalResolutionMs = 10;
};

inline bool Subscription::instant_publishing() const {
  return publishing_enabled_ && publishing_interval_ms_ < kMinPublishingIntervalResolutionMs;
}

} // namespace server
} // namespace opcua