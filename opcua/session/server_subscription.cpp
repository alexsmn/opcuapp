#include "opcua/session/server_subscription.h"

#include "opcua/server/endpoint_core.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace opcua {
namespace {

constexpr UInt32 kDefaultKeepAliveCount = 3;

}  // namespace

SubscriptionParameters ServerSubscription::ReviseParameters(
    SubscriptionParameters parameters) {
  if (parameters.max_keep_alive_count == 0) {
    parameters.max_keep_alive_count = kDefaultKeepAliveCount;
  }
  const UInt32 min_lifetime_count = 3 * parameters.max_keep_alive_count;
  if (parameters.lifetime_count < min_lifetime_count) {
    parameters.lifetime_count = min_lifetime_count;
  }
  return parameters;
}

ServerSubscription::ServerSubscription(
    SubscriptionId subscription_id,
    SubscriptionParameters parameters,
    AnyExecutor executor,
    ServiceCallbacks::CreateSubscriptionCallback create_subscription,
    base::Time publish_cycle_start_time)
    : subscription_id_{subscription_id},
      parameters_{ReviseParameters(std::move(parameters))},
      monitored_item_pump_{
          std::move(executor),
          std::move(create_subscription),
          {},
          [this](std::vector<scada::ItemNotification> notifications) {
            OnNotifications(std::move(notifications));
          },
          [this](Status status) { OnSubscriptionError(std::move(status)); }},
      last_publish_time_{publish_cycle_start_time} {}

ModifySubscriptionResponse ServerSubscription::Modify(
    const ModifySubscriptionRequest& request) {
  if (request.subscription_id != subscription_id_) {
    return {.status = StatusCode::Bad_WrongSubscriptionId};
  }

  parameters_ = ReviseParameters(request.parameters);
  return {.status = StatusCode::Good,
          .revised_publishing_interval_ms = parameters_.publishing_interval_ms,
          .revised_lifetime_count = parameters_.lifetime_count,
          .revised_max_keep_alive_count = parameters_.max_keep_alive_count};
}

void ServerSubscription::SetPublishingEnabled(bool publishing_enabled) {
  parameters_.publishing_enabled = publishing_enabled;
}

bool ServerSubscription::IsPublishReady(base::Time now) const {
  if (!last_publish_time_.has_value())
    return false;

  const auto elapsed = now - *last_publish_time_;
  if (initial_message_sent_ && parameters_.publishing_enabled &&
      !pending_notifications_.empty()) {
    return elapsed >= PublishingInterval();
  }

  if (!initial_message_sent_) {
    return elapsed >= PublishingInterval();
  }

  if (!parameters_.publishing_enabled || pending_notifications_.empty()) {
    return elapsed >= KeepAliveInterval();
  }

  if (!pending_notifications_.empty()) {
    return now - *last_publish_time_ >= PublishingInterval();
  }
  return elapsed >= KeepAliveInterval();
}

void ServerSubscription::PrimePublishCycle(base::Time now) {
  if (!parameters_.publishing_enabled || last_publish_time_.has_value())
    return;
  last_publish_time_ = now;
}

std::optional<base::Time> ServerSubscription::NextPublishDeadline() const {
  if (!last_publish_time_.has_value()) {
    return std::nullopt;
  }

  if (!initial_message_sent_) {
    return *last_publish_time_ + PublishingInterval();
  }

  return *last_publish_time_ +
         ((parameters_.publishing_enabled && !pending_notifications_.empty())
              ? PublishingInterval()
              : KeepAliveInterval());
}

CreateMonitoredItemsResponse ServerSubscription::CreateMonitoredItems(
    const CreateMonitoredItemsRequest& request) {
  if (request.subscription_id != subscription_id_) {
    return {.status = StatusCode::Bad_WrongSubscriptionId};
  }

  CreateMonitoredItemsResponse response{.status = StatusCode::Good};
  response.results.reserve(request.items_to_create.size());

  for (const auto& source_item : request.items_to_create) {
    auto item = std::make_shared<Item>();
    item->monitored_item_id = next_monitored_item_id_++;
    item->item_to_monitor = source_item.item_to_monitor;
    item->index_range = source_item.index_range;
    item->monitoring_mode = source_item.monitoring_mode;
    item->parameters = source_item.requested_parameters;

    items_.emplace(item->monitored_item_id, item);
    RebindItem(*item);

    response.results.push_back(
        {.status = item->monitored_item_status,
         .monitored_item_id = item->monitored_item_status == StatusCode::Good
                                  ? item->monitored_item_id
                                  : 0,
         .revised_sampling_interval_ms = item->parameters.sampling_interval_ms,
         .revised_queue_size =
             std::max<UInt32>(1, item->parameters.queue_size)});
    if (item->monitored_item_status != StatusCode::Good)
      items_.erase(item->monitored_item_id);
  }

  return response;
}

ModifyMonitoredItemsResponse ServerSubscription::ModifyMonitoredItems(
    const ModifyMonitoredItemsRequest& request) {
  if (request.subscription_id != subscription_id_) {
    return {.status = StatusCode::Bad_WrongSubscriptionId};
  }

  ModifyMonitoredItemsResponse response{.status = StatusCode::Good};
  response.results.reserve(request.items_to_modify.size());

  for (const auto& source_item : request.items_to_modify) {
    const auto item_it = items_.find(source_item.monitored_item_id);
    if (item_it == items_.end()) {
      response.results.push_back(
          {.status = StatusCode::Bad_MonitoredItemIdInvalid});
      continue;
    }

    auto& item = *item_it->second;
    item.parameters = source_item.requested_parameters;
    RebindItem(item);

    response.results.push_back(
        {.status = item.monitored_item_status,
         .revised_sampling_interval_ms = item.parameters.sampling_interval_ms,
         .revised_queue_size =
             std::max<UInt32>(1, item.parameters.queue_size)});
  }

  return response;
}

DeleteMonitoredItemsResponse ServerSubscription::DeleteMonitoredItems(
    const DeleteMonitoredItemsRequest& request) {
  if (request.subscription_id != subscription_id_) {
    return {.status = StatusCode::Bad_WrongSubscriptionId};
  }

  DeleteMonitoredItemsResponse response{.status = StatusCode::Good};
  response.results.reserve(request.monitored_item_ids.size());

  for (auto monitored_item_id : request.monitored_item_ids) {
    const auto erased = items_.erase(monitored_item_id);
    response.results.push_back(erased ? StatusCode::Good
                                      : StatusCode::Bad_MonitoredItemIdInvalid);
  }

  pending_notifications_.erase(
      std::remove_if(pending_notifications_.begin(),
                     pending_notifications_.end(),
                     [&](const auto& queued) {
                       return std::find(request.monitored_item_ids.begin(),
                                        request.monitored_item_ids.end(),
                                        queued.source_item_id) !=
                              request.monitored_item_ids.end();
                     }),
      pending_notifications_.end());

  return response;
}

SetMonitoringModeResponse ServerSubscription::SetMonitoringMode(
    const SetMonitoringModeRequest& request) {
  if (request.subscription_id != subscription_id_) {
    return {.status = StatusCode::Bad_WrongSubscriptionId};
  }

  SetMonitoringModeResponse response{.status = StatusCode::Good};
  response.results.reserve(request.monitored_item_ids.size());

  for (auto monitored_item_id : request.monitored_item_ids) {
    const auto item_it = items_.find(monitored_item_id);
    if (item_it == items_.end()) {
      response.results.push_back(StatusCode::Bad_MonitoredItemIdInvalid);
      continue;
    }

    item_it->second->monitoring_mode = request.monitoring_mode;
    response.results.push_back(StatusCode::Good);
  }

  return response;
}

std::vector<StatusCode> ServerSubscription::Acknowledge(
    const std::vector<UInt32>& sequence_numbers) {
  std::vector<StatusCode> results;
  results.reserve(sequence_numbers.size());
  for (const auto sequence_number : sequence_numbers)
    results.push_back(Acknowledge(sequence_number));
  return results;
}

std::optional<PublishResponse> ServerSubscription::TryPublish(base::Time now) {
  PrimePublishCycle(now);
  const bool has_publishable_notifications =
      parameters_.publishing_enabled && !pending_notifications_.empty();
  if (!has_publishable_notifications) {
    if (!IsPublishReady(now))
      return std::nullopt;

    last_publish_time_ = now;
    initial_message_sent_ = true;
    return PublishResponse{
        .status = StatusCode::Good,
        .subscription_id = subscription_id_,
        .results = {},
        .more_notifications = false,
        .notification_message = {.sequence_number = next_sequence_number_,
                                 .publish_time = now},
        .available_sequence_numbers = AvailableSequenceNumbers()};
  }

  if (!IsPublishReady(now))
    return std::nullopt;

  auto queued = std::move(pending_notifications_.front());
  pending_notifications_.pop_front();

  NotificationMessage notification_message{
      .sequence_number = next_sequence_number_++,
      .publish_time = now,
      .notification_data = {std::move(queued.notification)}};
  retransmit_queue_.push_back(notification_message);
  while (retransmit_queue_.size() > kMaxRetransmitQueueNotifications) {
    retransmit_queue_.pop_front();
  }
  last_publish_time_ = now;
  initial_message_sent_ = true;

  return PublishResponse{
      .status = StatusCode::Good,
      .subscription_id = subscription_id_,
      .results = {},
      .more_notifications = !pending_notifications_.empty(),
      .notification_message = std::move(notification_message),
      .available_sequence_numbers = AvailableSequenceNumbers()};
}

RepublishResponse ServerSubscription::Republish(UInt32 sequence_number) const {
  const auto it = std::find_if(
      retransmit_queue_.begin(), retransmit_queue_.end(),
      [&](const auto& notification_message) {
        return notification_message.sequence_number == sequence_number;
      });
  if (it == retransmit_queue_.end()) {
    return {.status = StatusCode::Bad_MessageNotAvailable};
  }
  return {.status = StatusCode::Good, .notification_message = *it};
}

StatusCode ServerSubscription::Acknowledge(UInt32 sequence_number) {
  const auto it = std::find_if(
      retransmit_queue_.begin(), retransmit_queue_.end(),
      [&](const auto& notification_message) {
        return notification_message.sequence_number == sequence_number;
      });
  // Acknowledging a sequence number the server does not hold (unknown or
  // already acknowledged) is Bad_SequenceNumberUnknown. OPC UA Part 4 §5.13.5
  // Publish, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  if (it == retransmit_queue_.end())
    return StatusCode::Bad_SequenceNumberUnknown;
  retransmit_queue_.erase(it);
  return StatusCode::Good;
}

std::vector<UInt32> ServerSubscription::AvailableSequenceNumbers() const {
  std::vector<UInt32> result;
  result.reserve(retransmit_queue_.size());
  for (const auto& notification_message : retransmit_queue_)
    result.push_back(notification_message.sequence_number);
  return result;
}

base::TimeDelta ServerSubscription::PublishingInterval() const {
  const auto interval_ms =
      static_cast<int64_t>(parameters_.publishing_interval_ms);
  return base::TimeDelta::FromMilliseconds(std::max<int64_t>(1, interval_ms));
}

base::TimeDelta ServerSubscription::KeepAliveInterval() const {
  const auto interval_ms =
      static_cast<int64_t>(PublishingInterval().InMilliseconds()) *
      static_cast<int64_t>(
          std::max<UInt32>(1, parameters_.max_keep_alive_count));
  return base::TimeDelta::FromMilliseconds(interval_ms);
}

void ServerSubscription::RebindItem(Item& item) {
  if (!IsSupportedMonitoredAttribute(item.item_to_monitor.attribute_id)) {
    item.monitored_item_status = StatusCode::Bad_WrongAttributeId;
    return;
  }

  const Status start_status = monitored_item_pump_.Start();
  if (!start_status) {
    item.monitored_item_status = start_status.code();
    return;
  }

  item.binding_requested = true;
  item.backing_item_id = 0;
  item.monitored_item_status = StatusCode::Good;
  item.backing_client_handle = next_backing_client_handle_++;

  MonitoringParameters parameters = item.parameters;
  parameters.client_handle = item.backing_client_handle;
  MonitoredItemCreateRequest request{
      .item_to_monitor = item.item_to_monitor,
      .index_range = item.index_range,
      .monitoring_mode = item.monitoring_mode,
      .requested_parameters = std::move(parameters)};

  const auto item_it = items_.find(item.monitored_item_id);
  const std::weak_ptr<Item> weak_item =
      item_it != items_.end() ? item_it->second : std::weak_ptr<Item>{};
  BindItem(std::move(weak_item), item.backing_client_handle,
           std::move(request));
}

void ServerSubscription::BindItem(std::weak_ptr<Item> weak_item,
                                  UInt32 backing_client_handle,
                                  MonitoredItemCreateRequest request) {
  CoSpawn(monitored_item_pump_.executor(),
          [this, weak_item = std::move(weak_item), backing_client_handle,
           request = std::move(request)]() mutable -> Awaitable<void> {
            std::vector<MonitoredItemCreateRequest> requests;
            requests.push_back(std::move(request));
            auto results =
                co_await monitored_item_pump_.AddItems(std::move(requests));
            OnBindResult(
                std::move(weak_item), backing_client_handle,
                results.empty()
                    ? MonitoredItemCreateResult{.status = StatusCode::Bad}
                    : std::move(results.front()));
          });
}

void ServerSubscription::OnBindResult(std::weak_ptr<Item> weak_item,
                                      UInt32 backing_client_handle,
                                      MonitoredItemCreateResult result) {
  auto item = weak_item.lock();
  if (!item || item->backing_client_handle != backing_client_handle)
    return;

  item->monitored_item_status = result.status.code();
  if (!result.status) {
    QueueItemStatus(*item, result.status);
    return;
  }

  item->backing_item_id = result.monitored_item_id;
}

void ServerSubscription::OnNotifications(
    std::vector<scada::ItemNotification> notifications) {
  for (auto& notification : notifications) {
    const UInt32 client_handle = std::visit(
        [](const auto& value) { return value.client_handle; }, notification);
    const auto item_it = std::find_if(
        items_.begin(), items_.end(), [client_handle](const auto& entry) {
          return entry.second->backing_client_handle == client_handle;
        });
    if (item_it == items_.end())
      continue;

    Item& item = *item_it->second;
    if (auto* data_change =
            std::get_if<MonitoredItemNotification>(&notification)) {
      QueueDataChange(item, data_change->value);
    } else if (auto* event = std::get_if<EventFieldList>(&notification)) {
      QueueEventFields(item, std::move(event->event_fields));
    }
  }
}

void ServerSubscription::OnSubscriptionError(Status status) {
  for (auto& [_, item] : items_) {
    QueueItemStatus(*item, status);
  }
}

bool ServerSubscription::PassesDeadband(const Item& item,
                                        const DataValue& data_value) {
  // OPC UA Part 4 §7.22.2 DataChangeFilter: an absolute deadband reports a
  // value only when it differs from the last reported value by at least the
  // deadband. https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.2
  const auto* filter =
      item.parameters.filter
          ? std::get_if<DataChangeFilter>(&*item.parameters.filter)
          : nullptr;
  if (!filter || filter->deadband_type != DeadbandType::Absolute ||
      filter->deadband_value <= 0) {
    return true;
  }
  if (!item.last_reported_value.has_value()) {
    return true;  // The first value is always reported.
  }
  // A status-code change is always reported regardless of the deadband.
  if (item.last_reported_value->status_code != data_value.status_code) {
    return true;
  }
  double previous = 0.0;
  double current = 0.0;
  if (!item.last_reported_value->value.get(previous) ||
      !data_value.value.get(current)) {
    return true;  // Non-numeric values are always reported.
  }
  return std::abs(current - previous) >= filter->deadband_value;
}

void ServerSubscription::QueueDataChange(Item& item,
                                         const DataValue& data_value) {
  if (item.monitoring_mode != MonitoringMode::Reporting)
    return;
  if (!PassesDeadband(item, data_value))
    return;
  item.last_reported_value = data_value;
  QueueNotification(
      item,
      DataChangeNotification{
          .monitored_items = {{.client_handle = item.parameters.client_handle,
                               .value = data_value}}});
}

void ServerSubscription::QueueEventFields(Item& item,
                                          std::vector<Variant> event_fields) {
  if (item.monitoring_mode != MonitoringMode::Reporting)
    return;
  QueueNotification(
      item, EventNotificationList{
                .events = {{.client_handle = item.parameters.client_handle,
                            .event_fields = std::move(event_fields)}}});
}

void ServerSubscription::QueueItemStatus(Item& item, Status status) {
  if (IsAttributeEventNotifier(item.item_to_monitor.attribute_id)) {
    QueueNotification(item, StatusChangeNotification{.status = status.code()});
    return;
  }
  QueueDataChange(item, DataValue{status.code(), base::Time::Now()});
}

void ServerSubscription::QueueNotification(Item& item,
                                           NotificationData notification) {
  pending_notifications_.push_back({.source_item_id = item.monitored_item_id,
                                    .notification = std::move(notification)});
  EnforceQueueLimit(item);
}

void ServerSubscription::EnforceQueueLimit(const Item& item) {
  const auto queue_size = std::max<UInt32>(1, item.parameters.queue_size);
  std::vector<size_t> indices;
  for (size_t i = 0; i < pending_notifications_.size(); ++i) {
    if (pending_notifications_[i].source_item_id == item.monitored_item_id)
      indices.push_back(i);
  }
  if (indices.size() <= queue_size)
    return;

  if (!item.parameters.discard_oldest) {
    pending_notifications_.erase(pending_notifications_.begin() +
                                 static_cast<std::ptrdiff_t>(indices.back()));
    return;
  }

  pending_notifications_.erase(pending_notifications_.begin() +
                               static_cast<std::ptrdiff_t>(indices.front()));
}

}  // namespace opcua
