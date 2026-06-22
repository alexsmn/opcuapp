#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/client/client_protocol_subscription.h"
#include "opcua/message.h"
#include "opcua/monitored/monitored_item.h"
#include "opcua/types/read_value_id.h"
#include "opcua/types/status.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace opcua {
class ClientSession;

// Qt-client-facing subscription wrapper. Creates a server-side OPC UA
// subscription lazily on first AddItems, then runs a background Publish loop
// so data-change notifications flow into the ReadNext queue.
class ClientSubscription
    : public std::enable_shared_from_this<ClientSubscription> {
 public:
  static std::shared_ptr<ClientSubscription> Create(ClientSession& session);

  ClientSubscription(const ClientSubscription&) = delete;
  ClientSubscription& operator=(const ClientSubscription&) = delete;
  ~ClientSubscription();

  [[nodiscard]] Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests);
  [[nodiscard]] Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids);
  [[nodiscard]] Awaitable<StatusOr<std::vector<ItemNotification>>> ReadNext(
      std::size_t max_count);
  void Close(Status status);

 private:
  explicit ClientSubscription(ClientSession& session);

  void EnsureCreated();
  void StartPublishLoop();
  void FlushPendingSubscriptions();
  void SpawnCreateMonitoredItem(std::uint32_t local_id,
                                ReadValueId read_value_id,
                                MonitoringParameters params,
                                std::uint32_t client_handle);
  void PushNotification(ItemNotification notification);

  struct PendingSubscription {
    std::uint32_t local_id = 0;
    ReadValueId read_value_id;
    MonitoringParameters params;
    std::uint32_t client_handle = 0;
  };

  ClientSession& session_;
  std::unique_ptr<ClientProtocolSubscription> impl_;
  bool is_creating_ = false;
  bool publish_loop_running_ = false;
  std::uint32_t next_local_id_ = 1;

  // Maps the MonitoredItem's local id to the server-assigned
  // MonitoredItemId, set once the create completes.
  std::unordered_map<std::uint32_t, MonitoredItemId> server_ids_by_local_id_;
  std::vector<PendingSubscription> pending_subscriptions_;
  std::mutex mutex_;
  std::deque<ItemNotification> pending_notifications_;
  Status close_status_ = StatusCode::Bad_Disconnected;
  bool closed_ = false;
};

}  // namespace opcua
