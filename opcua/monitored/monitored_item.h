#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <cstdint>
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
