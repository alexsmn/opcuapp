#include "subscription.h"

#include <opcuapp/assertions.h>
#include <opcuapp/extension_object.h>
#include <opcuapp/vector.h>

namespace opcua {
namespace server {

namespace {

void Copy(const OpcUa_NotificationMessage& source, OpcUa_NotificationMessage& target) {
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

} // namespace

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
  std::lock_guard<std::mutex> lock{mutex_};
  published_messages_.erase(sequence_number);
}

bool Subscription::Publish(PublishResponse& response) {
  std::lock_guard<std::mutex> lock{mutex_};

  if (notifications_.empty())
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

  {
    Vector<OpcUa_ExtensionObject> notifications(notifications_.size());
    for (size_t i = 0; i < notifications_.size(); ++i) {
      assert(IsValid(notifications_[i].get()));
      notifications_[i].Release(notifications[i]);
      assert(IsValid(notifications[i]));
    }
    notifications_.clear();

    NotificationMessage message;
    message.PublishTime = DateTime::UtcNow().get();
    message.SequenceNumber = next_sequence_number_++;
    message.NoOfNotificationData = static_cast<OpcUa_Int32>(notifications.size());
    message.NotificationData = notifications.release();
    assert(IsValid(message));

    Copy(message, response.NotificationMessage);
    response.MoreNotifications = !notifications_.empty() ? OpcUa_True : OpcUa_False;

    auto sequence_number = message.SequenceNumber;
    published_messages_.emplace(sequence_number, std::move(message));
  }

  assert(IsValid(response.NotificationMessage));
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

  OnNotification(ExtensionObject{Encode(data_change_notification)});
}

void Subscription::OnNotification(ExtensionObject&& notification) {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    notifications_.emplace_back(std::move(notification));
  }

  publish_available_handler_();
}

} // namespace server
} // names