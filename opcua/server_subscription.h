#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/message.h"
#include "opcua/service_callbacks.h"

#include "opcua/scada/legacy_monitored_item_adapter.h"

#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>

namespace opcua {

class ServerSubscription {
 public:
  ServerSubscription(
      SubscriptionId subscription_id,
      SubscriptionParameters parameters,
      AnyExecutor executor,
      ServiceCallbacks::CreateSubscriptionCallback create_subscription,
      base::Time publish_cycle_start_time);

  ServerSubscription(const ServerSubscription&) = delete;
  ServerSubscription& operator=(const ServerSubscription&) = delete;

  // Upper bound on the number of NotificationMessages retained for Republish.
  // When exceeded the oldest unacknowledged message is dropped (a later
  // Republish for it returns Bad_MessageNotAvailable). OPC UA Part 4 §5.13.5
  // Republish, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  static constexpr std::size_t kMaxRetransmitQueueNotifications = 1024;

  // Revises requested subscription parameters to the server's limits: a zero
  // keep-alive count gets a default, and the lifetime count is raised to at
  // least three times the keep-alive count. OPC UA Part 4 §5.13.2
  // CreateSubscription,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2
  [[nodiscard]] static SubscriptionParameters ReviseParameters(
      SubscriptionParameters parameters);

  SubscriptionId subscription_id() const { return subscription_id_; }
  const SubscriptionParameters& parameters() const { return parameters_; }
  bool HasPendingNotifications() const {
    return !pending_notifications_.empty();
  }
  base::TimeDelta PublishingInterval() const;
  bool IsPublishReady(base::Time now) const;
  void PrimePublishCycle(base::Time now);
  std::optional<base::Time> NextPublishDeadline() const;

  ModifySubscriptionResponse Modify(const ModifySubscriptionRequest& request);
  void SetPublishingEnabled(bool publishing_enabled);

  CreateMonitoredItemsResponse CreateMonitoredItems(
      const CreateMonitoredItemsRequest& request);
  ModifyMonitoredItemsResponse ModifyMonitoredItems(
      const ModifyMonitoredItemsRequest& request);
  DeleteMonitoredItemsResponse DeleteMonitoredItems(
      const DeleteMonitoredItemsRequest& request);
  SetMonitoringModeResponse SetMonitoringMode(
      const SetMonitoringModeRequest& request);

  std::vector<StatusCode> Acknowledge(
      const std::vector<UInt32>& sequence_numbers);
  std::optional<PublishResponse> TryPublish(base::Time now);
  RepublishResponse Republish(UInt32 sequence_number) const;

 private:
  struct Item {
    MonitoredItemId monitored_item_id = 0;
    ReadValueId item_to_monitor;
    std::optional<std::string> index_range;
    MonitoringMode monitoring_mode = MonitoringMode::Reporting;
    MonitoringParameters parameters;
    std::vector<std::vector<std::string>> event_field_paths;
    StatusCode monitored_item_status = StatusCode::Bad;
    std::shared_ptr<scada::MonitoredItem> monitored_item;
    UInt32 binding_generation = 0;
    // Last value queued for this item; used to apply the DataChangeFilter
    // absolute deadband.
    std::optional<DataValue> last_reported_value;
  };

  struct QueuedNotification {
    MonitoredItemId source_item_id = 0;
    NotificationData notification;
  };

  StatusCode Acknowledge(UInt32 sequence_number);
  std::vector<UInt32> AvailableSequenceNumbers() const;
  base::TimeDelta KeepAliveInterval() const;

  // True if `data_value` should be reported given the item's DataChangeFilter
  // absolute deadband (status changes and the first value always pass).
  static bool PassesDeadband(const Item& item, const DataValue& data_value);

  void RebindItem(Item& item);
  void QueueDataChange(Item& item, const DataValue& data_value);
  void QueueEvent(Item& item, const Status& status, const std::any& event);
  void QueueNotification(Item& item, NotificationData notification);
  void EnforceQueueLimit(const Item& item);

  static std::vector<std::vector<std::string>> ParseEventFieldPaths(
      const std::optional<MonitoringFilter>& filter);
  static std::vector<Variant> BuildEventFields(
      const std::vector<std::vector<std::string>>& field_paths,
      const std::any& event);

  SubscriptionId subscription_id_;
  SubscriptionParameters parameters_;
  // Bridges the service's subscription API to the single-item API used when
  // (re)binding monitored items. Declared before `items_` so it outlives the
  // `scada::MonitoredItem`s it creates (members destruct in reverse order).
  scada::LegacyMonitoredItemAdapter monitored_item_adapter_;

  UInt32 next_monitored_item_id_ = 1;
  UInt32 next_sequence_number_ = 1;

  bool initial_message_sent_ = false;
  std::optional<base::Time> last_publish_time_;

  std::unordered_map<MonitoredItemId, std::shared_ptr<Item>> items_;
  std::deque<QueuedNotification> pending_notifications_;
  std::deque<NotificationMessage> retransmit_queue_;
};

}  // namespace opcua
