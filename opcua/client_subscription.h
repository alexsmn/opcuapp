#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/client_protocol_subscription.h"
#include "opcua/message.h"
#include "opcua/scada/monitored_item.h"
#include "opcua/scada/read_value_id.h"
#include "opcua/scada/status.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace opcua {
class ClientSession;

// Qt-client-facing subscription wrapper. Creates a server-side OPC UA
// subscription lazily on first CreateMonitoredItem, then runs a background
// Publish loop so data-change notifications flow to the registered
// handlers.
class ClientSubscription
    : public std::enable_shared_from_this<ClientSubscription> {
 public:
  static std::shared_ptr<ClientSubscription> Create(
      ClientSession& session);

  ClientSubscription(const ClientSubscription&) = delete;
  ClientSubscription& operator=(const ClientSubscription&) = delete;
  ~ClientSubscription();

  [[nodiscard]] std::shared_ptr<scada::MonitoredItem> CreateMonitoredItem(
      const ReadValueId& read_value_id,
      const MonitoringParameters& params);

  // Invoked by MonitoredItem during Subscribe() to attach its handler
  // and launch the server-side monitored item.
  void Subscribe(std::uint32_t local_id,
                 const ReadValueId& read_value_id,
                 const MonitoringParameters& params,
                 scada::MonitoredItemHandler handler);

  // Invoked by MonitoredItem during destruction; removes the item
  // from the server and drops the handler.
  void Unsubscribe(std::uint32_t local_id);

 private:
  explicit ClientSubscription(ClientSession& session);

  void EnsureCreated();
  void StartPublishLoop();
  void FlushPendingSubscriptions();
  void SpawnCreateMonitoredItem(std::uint32_t local_id,
                                ReadValueId read_value_id,
                                MonitoringParameters params,
                                scada::DataChangeHandler dispatch);

  struct PendingSubscription {
    std::uint32_t local_id = 0;
    ReadValueId read_value_id;
    MonitoringParameters params;
    scada::DataChangeHandler dispatch;
  };

  ClientSession& session_;
  std::unique_ptr<ClientProtocolSubscription> impl_;
  bool is_creating_ = false;
  bool publish_loop_running_ = false;
  std::uint32_t next_local_id_ = 1;

  // Maps the MonitoredItem's local id to the server-assigned
  // MonitoredItemId, set once the create completes.
  std::unordered_map<std::uint32_t, MonitoredItemId>
      server_ids_by_local_id_;
  std::vector<PendingSubscription> pending_subscriptions_;
};

}  // namespace opcua
