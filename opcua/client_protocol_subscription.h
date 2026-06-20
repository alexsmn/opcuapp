#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/client_channel.h"
#include "opcua/message.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/read_value_id.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace opcua {

// Client-side OPC UA Subscription wrapper. Creates a server-side
// subscription, manages a bag of monitored items by client_handle, and
// drives the Publish service to pull data-change notifications off the
// server and dispatch them to per-item callbacks.
//
// Publish() keeps up to two PublishRequests outstanding, matching the common
// OPC UA client convention and reducing notification latency while preserving
// the existing "one response per await" API.
class ClientProtocolSubscription {
 public:
  using DataChangeHandler = std::function<void(scada::DataValue)>;

  explicit ClientProtocolSubscription(ClientChannel& channel);

  // Creates the server-side subscription. Must be called before any
  // monitored item operations.
  [[nodiscard]] Awaitable<scada::Status> Create(
      SubscriptionParameters parameters = {});

  [[nodiscard]] bool is_created() const { return is_created_; }
  [[nodiscard]] SubscriptionId subscription_id() const {
    return subscription_id_;
  }

  // Adds a monitored item. Returns the server's monitored_item_id. The
  // handler is retained and invoked for every subsequent data-change
  // notification whose client_handle matches the one assigned here.
  struct CreateMonitoredItemResult {
    MonitoredItemId monitored_item_id = 0;
    scada::UInt32 client_handle = 0;
  };
  [[nodiscard]] Awaitable<scada::StatusOr<CreateMonitoredItemResult>>
  CreateMonitoredItem(scada::ReadValueId read_value_id,
                      MonitoringParameters params,
                      DataChangeHandler handler);

  // Deletes a monitored item and drops its handler.
  [[nodiscard]] Awaitable<scada::Status> DeleteMonitoredItem(
      MonitoredItemId monitored_item_id);

  // Issues a single PublishRequest and dispatches every data-change
  // notification in the response to the registered handlers. The returned
  // status reflects the Publish service call itself (Good even when there
  // are no notifications); service faults surface as bad Status.
  [[nodiscard]] Awaitable<scada::Status> Publish();

  // Deletes the server-side subscription and drops all handlers.
  [[nodiscard]] Awaitable<scada::Status> Delete();

 private:
  struct OutstandingPublish {
    std::uint32_t request_id = 0;
    std::uint32_t request_handle = 0;
  };

  [[nodiscard]] Awaitable<scada::Status> FillPublishWindow();
  [[nodiscard]] Awaitable<scada::Status> SendPublishRequest();
  [[nodiscard]] scada::Status HandlePublishResponse(
      PublishResponse response);

  ClientChannel& channel_;
  bool is_created_ = false;
  SubscriptionId subscription_id_ = 0;
  scada::UInt32 next_client_handle_ = 1;
  std::unordered_map<scada::UInt32, DataChangeHandler> handlers_;
  std::unordered_map<MonitoredItemId, scada::UInt32>
      client_handle_by_item_id_;
  std::vector<SubscriptionAcknowledgement> pending_acks_;
  std::deque<OutstandingPublish> outstanding_publishes_;
};

}  // namespace opcua
