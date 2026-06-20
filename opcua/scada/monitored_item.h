#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/read_value_id.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace opcua {
namespace scada {

using MonitoredItemId = std::uint32_t;

struct MonitoredItemCreateRequest {
  ReadValueId item_to_monitor;
  MonitoringParameters parameters;
  std::uint32_t client_handle = 0;
};

struct MonitoredItemCreateResult {
  MonitoredItemId item_id = 0;
  std::uint32_t client_handle = 0;
  Status status = StatusCode::Bad;
};

struct MonitoredItemSubscriptionOptions {
  std::size_t max_pending_notifications = 65536;
  std::size_t max_batch_size = 1024;
};

struct DataChangeNotification {
  MonitoredItemId item_id = 0;
  std::uint32_t client_handle = 0;
  DataValue value;
};

struct EventNotification {
  MonitoredItemId item_id = 0;
  std::uint32_t client_handle = 0;
  Status status = StatusCode::Good;
  std::any event;
};

struct ItemStatusNotification {
  MonitoredItemId item_id = 0;
  std::uint32_t client_handle = 0;
  Status status = StatusCode::Bad;
};

struct OverflowNotification {
  Status status = StatusCode::Bad;
};

using MonitoredItemNotification =
    std::variant<DataChangeNotification,
                 EventNotification,
                 ItemStatusNotification,
                 OverflowNotification>;

// When first subscribed, a cached value is sent immediately if one is already
// available. If no cached value exists yet, the first callback is expected to
// arrive once the initial value or status becomes available from the source.
using DataChangeHandler = std::function<void(const DataValue& data_value)>;

// When first subscribed, a cached status update may be sent immediately if one
// is already available. Otherwise the first callback is expected once the
// initial status (and optional event payload) becomes available from the
// source. WARNING: `event` may be not set for the initial status update.
using EventHandler =
    std::function<void(const Status& status, const std::any& event)>;

using MonitoredItemHandler = std::variant<DataChangeHandler, EventHandler>;

class MonitoredItem {
 public:
  MonitoredItem() {}
  virtual ~MonitoredItem() = default;

  virtual void Subscribe(MonitoredItemHandler handler) = 0;
};

class MonitoredItemSubscription {
 public:
  virtual ~MonitoredItemSubscription() = default;

  virtual Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests) = 0;

  virtual Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids) = 0;

  virtual Awaitable<StatusOr<std::vector<MonitoredItemNotification>>> ReadNext(
      std::size_t max_count) = 0;

  virtual void Close(Status status) = 0;
};

}  // namespace scada
}  // namespace opcua (vendored)
