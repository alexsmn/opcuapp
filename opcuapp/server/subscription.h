#pragma once

#include <opcuapp/assertions.h>
#include <opcuapp/extension_object.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/vector.h>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

namespace opcua {

inline void Copy(const OpcUa_NotificationMessage& source,
                 OpcUa_NotificationMessage& target) {
  assert(IsValid(source));

  target.SequenceNumber = source.SequenceNumber;
  target.PublishTime = source.PublishTime;
  target.NoOfNotificationData = source.NoOfNotificationData;

  if (source.NoOfNotificationData != 0) {
    Vector<OpcUa_ExtensionObject> notifications{
        static_cast<size_t>(source.NoOfNotificationData)};
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
  const Double publishing_interval_ms_;
  const UInt32 max_lifetime_count_;
  const UInt32 max_keep_alive_count_;
  const size_t max_notifications_per_publish_;
  const bool publishing_enabled_;
  const Byte priority_;
  const CreateMonitoredItemHandler create_monitored_item_handler_;
  const std::function<void()> publish_handler_;
  const std::function<void()> close_handler_;
};

template <class Timer>
class BasicSubscription
    : public std::enable_shared_from_this<BasicSubscription<Timer>>,
      private SubscriptionContext {
 public:
  static std::shared_ptr<BasicSubscription> Create(
      SubscriptionContext&& context);

  SubscriptionId id() const { return id_; }

  template <class ResponseHandler>
  void BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request,
                   ResponseHandler&& response_handler);

  template <class ResponseHandler>
  void BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request,
                   ResponseHandler&& response_handler);

  bool Acknowledge(UInt32 sequence_number);
  bool Publish(PublishResponse& response);

  void Close();

 private:
  using WeakPtr = std::weak_ptr<BasicSubscription<Timer>>;

  explicit BasicSubscription(SubscriptionContext&& context);

  void Init();

  bool CloseInternal();

  bool instant_publishing() const;

  struct ItemData {
    MonitoredItemClientHandle client_handle;
    AttributeId attribute_id;
    EventFilter event_filter;
    std::shared_ptr<MonitoredItem> monitored_item;
  };

  ItemData CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request,
                               OpcUa_MonitoredItemCreateResult& result);
  StatusCode DeleteMonitoredItem(MonitoredItemId monitored_item_id);

  void OnDataChange(MonitoredItemClientHandle client_handle,
                    DataValue&& data_value);
  void OnEvent(MonitoredItemClientHandle client_handle,
               Vector<OpcUa_Variant>&& event_fields);
  void OnNotification(ExtensionObject&& notification);

  void OnPublishingTimer();

  bool PublishMessage(NotificationMessage& message);

  UInt32 MakeNextSequenceNumber();

  std::mutex mutex_;

  std::queue<ExtensionObject> notifications_;

  UInt32 keep_alive_count_ = 0;
  UInt32 lifetime_count_ = 0;

  UInt32 next_sequence_number_ = 1;
  std::map<UInt32 /*sequence_number*/, NotificationMessage> published_messages_;

  MonitoredItemId next_item_id_ = 1;
  std::map<MonitoredItemId, ItemData> items_;

  Timer publishing_timer_;

  bool closed_ = false;

  const Double kMinPublishingIntervalResolutionMs = 10;
};

// static
template <class Timer>
std::shared_ptr<BasicSubscription<Timer>> BasicSubscription<Timer>::Create(
    SubscriptionContext&& context) {
  auto result = std::shared_ptr<BasicSubscription<Timer>>(
      new BasicSubscription<Timer>{std::move(context)});
  result->Init();
  return result;
}

template <class Timer>
inline BasicSubscription<Timer>::BasicSubscription(
    SubscriptionContext&& context)
    : SubscriptionContext{std::move(context)} {}

template <class Timer>
void BasicSubscription<Timer>::Init() {
  if (!instant_publishing()) {
    auto ref = this->shared_from_this();
    publishing_timer_.set_interval(
        static_cast<UInt32>(publishing_interval_ms_));
    publishing_timer_.Start([ref] { ref->OnPublishingTimer(); });
  }
}

template <class Timer>
inline bool BasicSubscription<Timer>::instant_publishing() const {
  return publishing_interval_ms_ < kMinPublishingIntervalResolutionMs;
}

template <class Timer>
template <class ResponseHandler>
inline void BasicSubscription<Timer>::BeginInvoke(
    OpcUa_CreateMonitoredItemsRequest& request,
    ResponseHandler&& response_handler) {
  WeakPtr weak_ptr = this->shared_from_this();

  CreateMonitoredItemsResponse response;

  std::vector<ItemData> items;

  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (closed_) {
      response.ResponseHeader.ServiceResult = OpcUa_BadNoSubscription;

    } else {
      lifetime_count_ = 0;

      items.reserve(request.NoOfItemsToCreate);

      Vector<OpcUa_MonitoredItemCreateResult> results(
          request.NoOfItemsToCreate);
      for (size_t i = 0; i < static_cast<size_t>(request.NoOfItemsToCreate);
           ++i) {
        auto item = CreateMonitoredItem(request.ItemsToCreate[i], results[i]);
        if (item.monitored_item)
          items.emplace_back(std::move(item));
      }

      response.NoOfResults = results.size();
      response.Results = results.release();
    }
  }

  response_handler(std::move(response));

  for (auto& item : items) {
    auto client_handle = item.client_handle;
    if (item.attribute_id == OpcUa_Attributes_EventNotifier) {
      item.monitored_item->SubscribeEvents(
          item.event_filter,
          [weak_ptr, client_handle](Vector<OpcUa_Variant>&& event_fields) {
            if (auto ptr = weak_ptr.lock())
              ptr->OnEvent(client_handle, std::move(event_fields));
          });

    } else {
      item.monitored_item->SubscribeDataChange(
          [weak_ptr, client_handle](DataValue&& data_value) {
            if (auto ptr = weak_ptr.lock())
              ptr->OnDataChange(client_handle, std::move(data_value));
          });
    }
  }
}

template <class Timer>
template <class ResponseHandler>
inline void BasicSubscription<Timer>::BeginInvoke(
    OpcUa_DeleteMonitoredItemsRequest& request,
    ResponseHandler&& response_handler) {
  DeleteMonitoredItemsResponse response;

  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (closed_) {
      response.ResponseHeader.ServiceResult = OpcUa_BadNoSubscription;

    } else {
      lifetime_count_ = 0;

      Vector<OpcUa_StatusCode> results(request.NoOfMonitoredItemIds);
      for (size_t i = 0; i < static_cast<size_t>(request.NoOfMonitoredItemIds);
           ++i)
        results[i] = DeleteMonitoredItem(request.MonitoredItemIds[i]).code();
      response.NoOfResults = results.size();
      response.Results = results.release();
    }
  }

  response_handler(std::move(response));
}

template <class Timer>
inline bool BasicSubscription<Timer>::Acknowledge(UInt32 sequence_number) {
  std::lock_guard<std::mutex> lock{mutex_};
  lifetime_count_ = 0;
  return published_messages_.erase(sequence_number) != 0;
}

template <class Timer>
inline UInt32 BasicSubscription<Timer>::MakeNextSequenceNumber() {
  auto result = next_sequence_number_;
  if (++next_sequence_number_ == 0)
    next_sequence_number_ = 1;
  return result;
}

template <class Timer>
inline bool BasicSubscription<Timer>::PublishMessage(
    NotificationMessage& message) {
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

    message.NoOfNotificationData =
        static_cast<OpcUa_Int32>(notifications.size());
    message.NotificationData = notifications.release();

  } else if (instant_publishing() ||
             keep_alive_count_ >= max_keep_alive_count_) {
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

template <class Timer>
inline bool BasicSubscription<Timer>::Publish(PublishResponse& response) {
  std::lock_guard<std::mutex> lock{mutex_};

  if (closed_)
    return false;

  lifetime_count_ = 0;

  NotificationMessage message;
  if (!PublishMessage(message))
    return false;

  response.SubscriptionId = id_;

  if (!published_messages_.empty()) {
    Vector<OpcUa_UInt32> available_sequence_numbers{published_messages_.size()};
    size_t i = 0;
    for (auto& p : published_messages_)
      available_sequence_numbers[i++] = p.first;
    response.NoOfAvailableSequenceNumbers =
        static_cast<OpcUa_Int32>(available_sequence_numbers.size());
    response.AvailableSequenceNumbers = available_sequence_numbers.release();
  }

  Copy(message, response.NotificationMessage);
  response.MoreNotifications =
      !notifications_.empty() ? OpcUa_True : OpcUa_False;

  auto sequence_number = message.SequenceNumber;
  published_messages_.emplace(sequence_number, std::move(message));

  assert(IsValid(response.NotificationMessage));
  return true;
}

template <class Timer>
inline typename BasicSubscription<Timer>::ItemData
BasicSubscription<Timer>::CreateMonitoredItem(
    OpcUa_MonitoredItemCreateRequest& request,
    OpcUa_MonitoredItemCreateResult& result) {
  assert(create_monitored_item_handler_);

  auto client_handle = request.RequestedParameters.ClientHandle;
  ReadValueId read_value_id{std::move(request.ItemToMonitor)};
  MonitoringParameters params{std::move(request.RequestedParameters)};

  auto create_result = create_monitored_item_handler_(std::move(read_value_id),
                                                      std::move(params));
  if (create_result.status_code.IsBad()) {
    result.StatusCode = create_result.status_code.code();
    return ItemData{};
  }

  auto item_id = next_item_id_++;
  auto& data = items_[item_id];
  data.client_handle = client_handle;
  data.attribute_id = read_value_id.AttributeId;
  if (read_value_id.AttributeId == OpcUa_Attributes_EventNotifier) {
    opcua::ExtensionObject filter{
        std::move(request.RequestedParameters.Filter)};
    if (auto* event_filter = filter.get_if<OpcUa_EventFilter>())
      data.event_filter = std::move(*event_filter);
  }
  data.monitored_item = std::move(create_result.monitored_item);

  result.MonitoredItemId = item_id;
  result.RevisedQueueSize = request.RequestedParameters.QueueSize;
  result.RevisedSamplingInterval = request.RequestedParameters.SamplingInterval;
  result.StatusCode = OpcUa_Good;

  return data;
}

template <class Timer>
inline StatusCode BasicSubscription<Timer>::DeleteMonitoredItem(
    MonitoredItemId monitored_item_id) {
  auto i = items_.find(monitored_item_id);
  if (i == items_.end())
    return OpcUa_BadInvalidArgument;

  items_.erase(i);

  return OpcUa_Good;
}

template <class Timer>
inline void BasicSubscription<Timer>::OnDataChange(
    MonitoredItemClientHandle client_handle,
    DataValue&& data_value) {
  Vector<OpcUa_MonitoredItemNotification> monitored_items{1};
  monitored_items[0].ClientHandle = client_handle;
  data_value.release(monitored_items[0].Value);

  DataChangeNotification data_change_notification;
  data_change_notification.NoOfMonitoredItems =
      static_cast<OpcUa_Int32>(monitored_items.size());
  data_change_notification.MonitoredItems = monitored_items.release();

  OnNotification(ExtensionObject::Encode(std::move(data_change_notification)));
}

template <class Timer>
inline void BasicSubscription<Timer>::OnEvent(
    MonitoredItemClientHandle client_handle,
    Vector<OpcUa_Variant>&& event_fields) {
  Vector<OpcUa_EventFieldList> events{1};
  events[0].ClientHandle = client_handle;
  events[0].NoOfEventFields = event_fields.size();
  events[0].EventFields = event_fields.release();

  EventNotificationList event_notification_list;
  event_notification_list.NoOfEvents = static_cast<OpcUa_Int32>(events.size());
  event_notification_list.Events = events.release();

  OnNotification(ExtensionObject::Encode(std::move(event_notification_list)));
}

template <class Timer>
inline void BasicSubscription<Timer>::OnNotification(
    ExtensionObject&& notification) {
  assert(IsValid(notification.get()));

  {
    std::lock_guard<std::mutex> lock{mutex_};

    if (closed_)
      return;

    notifications_.emplace(std::move(notification));
  }

  if (publishing_enabled_ && instant_publishing())
    publish_handler_();
}

template <class Timer>
inline void BasicSubscription<Timer>::OnPublishingTimer() {
  assert(publishing_enabled_);
  assert(!instant_publishing());

  {
    std::unique_lock<std::mutex> lock{mutex_};

    if (closed_)
      return;

    // |lifetime_count| has +1 increased value, counting all publishing timer
    // events, even when there is queued publish request. In such case the
    // following Publish() call, most probably made from withing
    // |publish_handler_()| below, will clear the counter.
    if (++lifetime_count_ > max_lifetime_count_) {
      if (CloseInternal()) {
        lock.unlock();
        close_handler_();
      }
      return;
    }

    if (!publishing_enabled_ || notifications_.empty()) {
      ++keep_alive_count_;
      return;
    }
  }

  publish_handler_();
}

template <class Timer>
inline void BasicSubscription<Timer>::Close() {
  CloseInternal();
}

template <class Timer>
inline bool BasicSubscription<Timer>::CloseInternal() {
  if (closed_)
    return false;

  closed_ = false;
  publishing_timer_.Stop();
  return true;
}

}  // namespace server
}  // namespace opcua
