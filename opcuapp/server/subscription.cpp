#include "subscription.h"

#include <opcuapp/extension_object.h>

namespace opcua {
namespace server {

namespace {

void Copy(const OpcUa_NotificationMessage& source, OpcUa_NotificationMessage& target) {
  target.SequenceNumber = source.SequenceNumber;
  target.PublishTime = source.PublishTime;
  if (source.NoOfNotificationData != 0) {
    Vector<OpcUa_ExtensionObject> notifications{static_cast<size_t>(source.NoOfNotificationData)};
    for (size_t i = 0; i < notifications.size(); ++i)
      opcua::Copy(source.NotificationData[i], notifications[i]);
    target.NoOfNotificationData = static_cast<OpcUa_Int32>(notifications.size());
    target.NotificationData = notifications.release();
  }
}

} // namespace

/*ExtensionObject Subscription::Notification::Serialize() const {
  Vector<OpcUa_MonitoredItemNotification> monitored_items{1};
  monitored_items[0].ClientHandle = client_handle;
  opcua::Copy(data_value.get(), monitored_items[0].Value);

  DataChangeNotification data_change_notification;
  data_change_notification.NoOfMonitoredItems = static_cast<OpcUa_Int32>(monitored_items.size());
  data_change_notification.MonitoredItems = monitored_items.release();
  return ExtensionObject{data_change_notification.Encode()};
}

void Subscription::NotificationMessage::Serialize(OpcUa_NotificationMessage& target) const {
  Vector<OpcUa_ExtensionObject> target_notifications{notifications.size()};
  for (size_t i = 0; i < notifications.size(); ++i)
    notifications[i].Serialize().Release(target_notifications[i]);

  target.PublishTime = publish_time.get();
  target.SequenceNumber = sequence_number;
  target.NoOfNotificationData = static_cast<OpcUa_Int32>(target_notifications.size());
  target.NotificationData = target_notifications.release();
}*/

// Subscription

void Subscription::BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request,
    const std::function<void(OpcUa_CreateMonitoredItemsResponse& response)>& callback) {
  CreateMonitoredItemsResponse response;
  Vector<OpcUa_MonitoredItemCreateResult> results(request.NoOfItemsToCreate);
  for (size_t i = 0; i < static_cast<size_t>(request.NoOfItemsToCreate); ++i)
    CreateMonitoredItem(request.ItemsToCreate[i]).release(results[i]);
  response.NoOfResults = results.size();
  response.Results = results.release();
  callback(response);
}

void Subscription::BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request,
    const std::function<void(OpcUa_DeleteMonitoredItemsResponse& response)>& callback) {
  DeleteMonitoredItemsResponse response;
  Vector<OpcUa_StatusCode> results(request.NoOfMonitoredItemIds);
  for (size_t i = 0; i < static_cast<size_t>(request.NoOfMonitoredItemIds); ++i)
    results[i] = DeleteMonitoredItem(request.MonitoredItemIds[i]).code();
  response.NoOfResults = results.size();
  response.Results = results.release();
  callback(response);
}

void Subscription::Acknowledge(UInt32 sequence_number) {
  published_notifications_.erase(sequence_number);
}

bool Subscription::Publish(PublishResponse& response) {
  if (notifications_.empty())
    return false;

  response.SubscriptionId = id_;

  if (!published_notifications_.empty()) {
    Vector<OpcUa_UInt32> available_sequence_numbers{published_notifications_.size()};
    size_t i = 0;
    for (auto& p : published_notifications_)
      available_sequence_numbers[i++] = p.first;
    response.NoOfAvailableSequenceNumbers = static_cast<OpcUa_Int32>(available_sequence_numbers.size());
    response.AvailableSequenceNumbers = available_sequence_numbers.release();
  }

  {
    Vector<OpcUa_ExtensionObject> notifications(notifications_.size());
    for (size_t i = 0; i < notifications_.size(); ++i)
      notifications_[i].Release(notifications[i]);
    notifications_.clear();

    NotificationMessage message;
    message.PublishTime = DateTime::UtcNow().get();
    message.SequenceNumber = next_sequence_number_++;
    message.NoOfNotificationData = static_cast<OpcUa_Int32>(notifications.size());
    message.NotificationData = notifications.release();

    Copy(message, response.NotificationMessage);
    response.MoreNotifications = !notifications_.empty() ? OpcUa_True : OpcUa_False;

    auto sequence_number = message.SequenceNumber;
    published_notifications_.emplace(sequence_number, std::move(message));
  }

  return true;
}

MonitoredItemCreateResult Subscription::CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request) {
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

StatusCode Subscription::DeleteMonitoredItem(MonitoredItemId monitored_item_id) {
  auto i = items_.find(monitored_item_id);
  if (i == items_.end())
    return OpcUa_BadInvalidArgument;

  items_.erase(i);

  return OpcUa_Good;
}

void Subscription::OnDataChange(MonitoredItemClientHandle client_handle, DataValue&& data_value) {
  Vector<OpcUa_MonitoredItemNotification> monitored_items{1};
  monitored_items[0].ClientHandle = client_handle;
  opcua::Copy(data_value.get(), monitored_items[0].Value);

  DataChangeNotification data_change_notification;
  data_change_notification.NoOfMonitoredItems = static_cast<OpcUa_Int32>(monitored_items.size());
  data_change_notification.MonitoredItems = monitored_items.release();

  notifications_.emplace_back(ExtensionObject{Encode(data_change_notification)});
}

} // namespace server
} // names