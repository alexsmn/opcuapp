#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/types/data_value.h"
#include "opcua/types/read_value_id.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace opcua {
namespace scada {

// MonitoredItemId, MonitoredItemCreateRequest and MonitoredItemCreateResult are
// opcua's own (wire) types from message.h; the service operates on those
// directly. The bridge translates core scada:: <-> these.
//
// Server-assigned identifier of a MonitoredItem (an IntegerId). OPC UA Part 4
// §7.19 IntegerId,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.19
using opcua::MonitoredItemId;

// opcuapp tuning options for a `MonitoredItemSubscription` (notification queue
// bound and read batch size); an implementation detail of this service layer
// over the MonitoredItem Service Set. OPC UA Part 4 §5.13 MonitoredItem Service
// Set, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
struct MonitoredItemSubscriptionOptions {
  std::size_t max_pending_notifications = 65536;
  std::size_t max_batch_size = 1024;
};

// The notification stream uses opcua's own (wire) types from message.h, found
// here via enclosing-namespace lookup. Consumers correlate items by
// client_handle.
//   - `MonitoredItemNotification{client_handle, value}` carries a data-change
//     DataValue.
//   - `EventFieldList{client_handle, event_fields}` carries an event already
//     projected onto the monitored item's EventFilter select clauses.
// Both are standard OPC UA Part-4 types; no domain abstraction crosses this
// boundary. OPC UA Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
using opcua::EventFieldList;
using opcua::MonitoredItemNotification;

// A single notification on the `ReadNext` stream: either a data-change
// (`MonitoredItemNotification`) or a projected event (`EventFieldList`). The
// variant is the stream discriminator; both alternatives are standard wire
// types. OPC UA Part 4 §7.25 NotificationData,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.25
using ItemNotification =
    std::variant<opcua::MonitoredItemNotification, opcua::EventFieldList>;

// opcuapp callback delivering a MonitoredItem's data-change notifications,
// the service-layer counterpart of MonitoredItem reporting. OPC UA Part 4 §5.13
// MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
//
// When first subscribed, a cached value is sent immediately if one is already
// available. If no cached value exists yet, the first callback is expected to
// arrive once the initial value or status becomes available from the source.
using DataChangeHandler = std::function<void(const DataValue& data_value)>;

// opcuapp callback delivering a MonitoredItem's event notifications, the
// service-layer counterpart of event MonitoredItem reporting. OPC UA Part 4
// §5.13 MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
//
// When first subscribed, a cached status update may be sent immediately if one
// is already available. Otherwise the first callback is expected once the
// initial status (and optional event payload) becomes available from the
// source. WARNING: `event` may be not set for the initial status update.
using EventHandler =
    std::function<void(const Status& status, const std::any& event)>;

// opcuapp handler for a single MonitoredItem: either a data-change or an event
// handler, selected by the item's attribute. OPC UA Part 4 §5.13 MonitoredItem
// Service Set, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
using MonitoredItemHandler = std::variant<DataChangeHandler, EventHandler>;

// opcuapp service-layer abstraction of a single MonitoredItem; callers attach a
// handler to receive its notifications. OPC UA Part 4 §5.13 MonitoredItem
// Service Set, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
class MonitoredItem {
 public:
  MonitoredItem() {}
  virtual ~MonitoredItem() = default;

  virtual void Subscribe(MonitoredItemHandler handler) = 0;
};

// opcuapp service-layer abstraction over the MonitoredItem Service Set: adds
// and removes monitored items and delivers their notifications as
// coroutine-read batches. OPC UA Part 4 §5.13 MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
class MonitoredItemSubscription {
 public:
  virtual ~MonitoredItemSubscription() = default;

  virtual Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests) = 0;

  virtual Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids) = 0;

  virtual Awaitable<StatusOr<std::vector<ItemNotification>>> ReadNext(
      std::size_t max_count) = 0;

  virtual void Close(Status status) = 0;
};

}  // namespace scada
}  // namespace opcua
