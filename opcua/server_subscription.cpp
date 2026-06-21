#include "opcua/server_subscription.h"

#include "opcua/endpoint_core.h"

#include "opcua/scada/event.h"
#include "opcua/scada/monitored_item.h"

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
    scada::MonitoredItemService& monitored_item_service,
    base::Time publish_cycle_start_time)
    : subscription_id_{subscription_id},
      parameters_{ReviseParameters(std::move(parameters))},
      monitored_item_adapter_{std::move(executor), monitored_item_service},
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
    item->event_field_paths =
        ParseEventFieldPaths(source_item.requested_parameters.filter);

    items_.emplace(item->monitored_item_id, item);
    RebindItem(*item);

    response.results.push_back(
        {.status = item->monitored_item_status,
         .monitored_item_id =
             item->monitored_item ? item->monitored_item_id : 0,
         .revised_sampling_interval_ms = item->parameters.sampling_interval_ms,
         .revised_queue_size =
             std::max<UInt32>(1, item->parameters.queue_size)});
    if (!item->monitored_item)
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
    item.event_field_paths =
        ParseEventFieldPaths(source_item.requested_parameters.filter);
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
    response.results.push_back(
        erased ? StatusCode::Good
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

RepublishResponse ServerSubscription::Republish(
    UInt32 sequence_number) const {
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

StatusCode ServerSubscription::Acknowledge(
    UInt32 sequence_number) {
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

std::vector<UInt32> ServerSubscription::AvailableSequenceNumbers()
    const {
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
  ++item.binding_generation;
  auto created =
      CreateMonitoredItem(monitored_item_adapter_, item.item_to_monitor,
                          item.parameters);
  item.monitored_item = std::move(created.monitored_item);
  item.monitored_item_status = created.status;
  if (!item.monitored_item)
    return;

  const auto item_it = items_.find(item.monitored_item_id);
  const std::weak_ptr<Item> weak_item =
      item_it != items_.end() ? item_it->second : std::weak_ptr<Item>{};
  const auto binding_generation = item.binding_generation;
  item.monitored_item->Subscribe(MakeMonitoredItemHandler(
      item.item_to_monitor,
      [this, weak_item,
       binding_generation](const DataValue& data_value) {
        const auto item = weak_item.lock();
        if (!item || item->binding_generation != binding_generation)
          return;
        QueueDataChange(*item, data_value);
      },
      [this, weak_item, binding_generation](const Status& status,
                                            const std::any& event) {
        const auto item = weak_item.lock();
        if (!item || item->binding_generation != binding_generation)
          return;
        QueueEvent(*item, status, event);
      }));
}

bool ServerSubscription::PassesDeadband(const Item& item,
                                        const DataValue& data_value) {
  // OPC UA Part 4 §7.22.2 DataChangeFilter: an absolute deadband reports a value
  // only when it differs from the last reported value by at least the deadband.
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.2
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

void ServerSubscription::QueueEvent(Item& item,
                                    const Status& status,
                                    const std::any& event) {
  if (!status) {
    QueueNotification(item, StatusChangeNotification{.status = status.code()});
    return;
  }
  if (item.monitoring_mode != MonitoringMode::Reporting)
    return;
  QueueNotification(
      item, EventNotificationList{
                .events = {{.client_handle = item.parameters.client_handle,
                            .event_fields = BuildEventFields(
                                item.event_field_paths, event)}}});
}

void ServerSubscription::QueueNotification(Item& item,
                                           NotificationData notification) {
  pending_notifications_.push_back({.source_item_id = item.monitored_item_id,
                                    .notification = std::move(notification)});
  EnforceQueueLimit(item);
}

void ServerSubscription::EnforceQueueLimit(const Item& item) {
  const auto queue_size =
      std::max<UInt32>(1, item.parameters.queue_size);
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

std::vector<std::vector<std::string>> ServerSubscription::ParseEventFieldPaths(
    const std::optional<MonitoringFilter>& filter) {
  const auto* raw_filter =
      filter ? std::get_if<boost::json::value>(&*filter) : nullptr;
  if (!raw_filter)
    return DefaultEventFieldPaths();
  return ParseEventFilterFieldPaths(*raw_filter);
}

std::vector<Variant> ServerSubscription::BuildEventFields(
    const std::vector<std::vector<std::string>>& field_paths,
    const std::any& event) {
  return ProjectEventFields(field_paths, event);
}

}  // namespace opcua
