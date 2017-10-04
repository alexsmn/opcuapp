#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <opcuapp/assertions.h>
#include <opcuapp/extension_object.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/vector.h>
#include <queue>
#include <vector>

namespace opcua {

inline void Copy(const OpcUa_NotificationMessage& source, OpcUa_NotificationMessage& target) {
  assert(IsValid(source));

  target.SequenceNumber = source.SequenceNumber;
  target.PublishTime = source.PublishTime;
  target.NoOfNotificationData = source.NoOfNotificationData;

  if (source.NoOfNotificationData != 0) {
    Vector<OpcUa_ExtensionObject> notifications{static_cast<size_t>(source.NoOfNotificationData)};
    for (size_t i = 0; i < notifications.size(); ++i) {
      assert(IsValid(source.NotificationData[i]));
      opcua::Copy(source.NotificationData[i], notifications[i]);
      assert(IsValid(notifications[i]));
    }
    target.NotificationData = notifications.release();
  }

  assert(IsValid(target));
}

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

template<class Timer>
class BasicSubscription : private SubscriptionContext {
 public:
  explicit BasicSubscription(SubscriptionContext&& context);

  SubscriptionId id() const { return id_; }

  template<class ResponseHandler>
  void BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request, ResponseHandler&& response_handler);

  template<class ResponseHandler>
  void BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request, ResponseHandler&& response_handler);

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

template<class Timer>
inline BasicSubscription<Timer>::BasicSubscription(SubscriptionContext&& context)
    : SubscriptionContext{std::move(context)} {
  if (!instant_publishing()) {
    publishing_timer_.set_interval(static_cast<UInt32>(publishing_interval_ms_));
    publishing_timer_.Start([this] { OnPublishingTimer(); });
  }
}

template<class Timer>
inline bool BasicSubscription<Timer>::instant_publishing() const {
  return publishing_interval_ms_ < kMinPublishingIntervalResolutionMs;
}

template<class Timer>
template<class ResponseHandler>
inline void BasicSubscription<Timer>::BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request, ResponseHandler&& response_handler) {
  CreateMonitoredItemsResponse response;
  Vector<OpcUa_MonitoredItemCreateResult> results(request.NoOfItemsToCreate);
  for (size_t i = 0; i < static_cast<size_t>(request.NoOfItemsToCreate); ++i)
    CreateMonitoredItem(request.ItemsToCreate[i]).release(results[i]);
  response.NoOfResults = results.size();
  response.Results = results.release();
  response_handler(std::move(response));
}

template<class Timer>
template<class ResponseHandler>
inline void BasicSubscription<Timer>::BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request, ResponseHandler&& response_handler) {
  DeleteMonitoredItemsResponse response;
  Vector<OpcUa_StatusCode> results(request.NoOfMonitoredItemIds);
  for (size_t i = 0; i < static_cast<size_t>(request.NoOfMonitoredItemIds); ++i)
    results[i] = DeleteMonitoredItem(request.MonitoredItemIds[i]).code();
  response.NoOfResults = results.size();
  response.Results = results.release();
  response_handler(std::move(response));
}

template<class Timer>
inline void BasicSubscription<Timer>::Acknowledge(UInt32 sequence_number) {
  std::lock_guard<std::mutex> lock{mutex_};
  published_messages_.erase(sequence_number);
}

template<class Timer>
inline UInt32 BasicSubscription<Timer>::MakeNextSequenceNumber() {
  auto result = next_sequence_number_;
  if (++next_sequence_number_ == 0)
    next_sequence_number_ = 1;
  return result;
}

template<class Timer>
inline bool BasicSubscription<Timer>::PublishMessage(NotificationMessage& message) {
  if (!notifications_.empty()) {
    // Notification messages response.
    Vector<OpcUa_ExtensionObject> notifications(
        std::min(max_notifications_per_publish_, notifications_.size()));
    for (size_t i = 0; i < notifications.size(); ++i) {
      assert(IsValid(notifications_.front().get()));
      auto notification = std::move(notifications_.front());
      notifications_.pop();
      assert(IsValid(notification.get()));
      notification.release(notifications[i]);
      assert(IsValid(notifications[i]));
    }

    message.NoOfNotificationData = static_cast<OpcUa_Int32>(notifications.size());
    message.NotificationData = notifications.release();

  } else if (instant_publishing() || keep_alive_count_ >= max_keep_alive_count_) {
    // Keep-alive response.
    keep_alive_count_ = 0;

  } else {
    return false;
  }

  message.PublishTime = DateTime::UtcNow().get();
  message.SequenceNumber = MakeNextSequenceNumber();
  assert(IsValid(message));

  return true;
}

template<class Timer>
inline bool BasicSubscription<Timer>::Publish(PublishResponse& response) {
  std::lock_guard<std::mutex> lock{mutex_};

  NotificationMessage message;
  if (!PublishMessage(message))
    return false;

  response.SubscriptionId = id_;

  if (!published_messages_.empty()) {
    Vector<OpcUa_UInt32> available_sequence_numbers{published_messages_.size()};
    size_t i = 0;
    for (auto& p : published_messages_)
      available_sequence_numbers[i++] = p.first;
    response.NoOfAvailableSequenceNumbers = static_cast<OpcUa_Int32>(available_sequence_numbers.size());
    response.AvailableSequenceNumbers = available_sequence_numbers.release();
  }

  NotificationMessage{message}.release(response.NotificationMessage);
  response.MoreNotifications = !notifications_.empty() ? OpcUa_True : OpcUa_False;

  auto sequence_number = message.SequenceNumber;
  published_messages_.emplace(sequence_number, std::move(message));

  assert(IsValid(response.NotificationMessage));
  return true;
}

template<class Timer>
inline MonitoredItemCreateResult BasicSubscription<Timer>::CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request) {
  assert(create_monitored_item_handler_);

  auto client_handle = request.RequestedParameters.ClientHandle;
  ReadValueId read_value_id{std::move(request.ItemToMonitor)};

  auto create_result = create_monitored_item_handler_(std::move(read_value_id));
  if (create_result.status_code.IsBad()) {
    MonitoredItemCreateResult result;
    result.StatusCode = create_result.status_code.code();
    return result;
  }

  auto item_id = next_item_id_++;
  auto& data = items_[item_id];
  data.client_handle = client_handle;
  data.item = std::move(create_result.monitored_item);
  data.item->Subscribe([this, client_handle](const DataValue& data_value) {
    OnDataChange(client_handle, DataValue{data_value});
  });

  MonitoredItemCreateResult result;
  result.MonitoredItemId = item_id;
  result.RevisedQueueSize = request.RequestedParameters.QueueSize;
  result.RevisedSamplingInterval = request.RequestedParameters.SamplingInterval;
  result.StatusCode = OpcUa_Good;
  return result;
}

template<class Timer>
inline StatusCode BasicSubscription<Timer>::DeleteMonitoredItem(MonitoredItemId monitored_item_id) {
  auto i = items_.find(monitored_item_id);
  if (i == items_.end())
    return OpcUa_BadInvalidArgument;

  items_.erase(i);

  return OpcUa_Good;
}

template<class Timer>
inline void BasicSubscription<Timer>::OnDataChange(MonitoredItemClientHandle client_handle, DataValue&& data_value) {
  Vector<OpcUa_MonitoredItemNotification> monitored_items{1};
  monitored_items[0].ClientHandle = client_handle;
  opcua::Copy(data_value.get(), monitored_items[0].Value);

  DataChangeNotification data_change_notification;
  data_change_notification.NoOfMonitoredItems = static_cast<OpcUa_Int32>(monitored_items.size());
  data_change_notification.MonitoredItems = monitored_items.release();

  OnNotification(ExtensionObject::Encode(std::move(data_change_notification)));
}

template<class Timer>
inline void BasicSubscription<Timer>::OnNotification(ExtensionObject&& notification) {
  assert(IsValid(notification.get()));

  {
    std::lock_guard<std::mutex> lock{mutex_};
    notifications_.emplace(std::move(notification));
  }

  if (publishing_enabled_ && instant_publishing())
    publish_handler_();
}

template<class Timer>
inline void BasicSubscription<Timer>::OnPublishingTimer() {
  assert(publishing_enabled_);
  assert(!instant_publishing());

  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!publishing_enabled_ || notifications_.empty()) {
      ++keep_alive_count_;
      return;
    }
  }

  publish_handler_();
}

} // namespace server
} // namespace opcua