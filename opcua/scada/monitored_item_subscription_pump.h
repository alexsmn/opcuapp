#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/scada/monitored_item.h"
#include "opcua/scada/monitored_item_service.h"
#include "opcua/scada/status.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace opcua {
namespace scada {

// Owns a `MonitoredItemSubscription` and continuously drains notification
// batches on the supplied executor. Callers keep domain-specific item-id
// mapping and notification interpretation outside this helper.
class MonitoredItemSubscriptionPump {
 public:
  using NotificationBatchHandler =
      std::function<void(std::vector<ItemNotification> notifications)>;
  using ErrorHandler = std::function<void(Status status)>;

  // `notification_batch_handler` and `error_handler` are invoked on `executor`.
  MonitoredItemSubscriptionPump(
      AnyExecutor executor,
      MonitoredItemService& monitored_item_service,
      MonitoredItemSubscriptionOptions options,
      NotificationBatchHandler notification_batch_handler,
      ErrorHandler error_handler);
  ~MonitoredItemSubscriptionPump();

  MonitoredItemSubscriptionPump(const MonitoredItemSubscriptionPump&) = delete;
  MonitoredItemSubscriptionPump& operator=(
      const MonitoredItemSubscriptionPump&) = delete;

  // Creates the underlying subscription and starts the read loop.
  [[nodiscard]] Status Start();

  // Adds monitored items through the owned subscription. If the pump is closed
  // or has not started, all requests complete with Bad_Disconnected.
  [[nodiscard]] Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests);

  // Removes monitored items through the owned subscription. If the pump is
  // closed or has not started, all ids complete with Bad_Disconnected.
  [[nodiscard]] Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids);

  // Closes the underlying subscription and stops future handler delivery.
  void Close(Status status);

 private:
  struct State;

  [[nodiscard]] static Awaitable<void> ReadLoop(std::shared_ptr<State> state);

  const AnyExecutor executor_;
  MonitoredItemService& monitored_item_service_;
  const MonitoredItemSubscriptionOptions options_;
  NotificationBatchHandler notification_batch_handler_;
  ErrorHandler error_handler_;
  std::shared_ptr<State> state_;
};

}  // namespace scada
}  // namespace opcua (vendored)
