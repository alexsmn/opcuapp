#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/client/client_protocol_subscription.h"
#include "opcua/message.h"
#include "opcua/monitored/monitored_item.h"
#include "opcua/types/co_result.h"
#include "opcua/types/read_value_id.h"
#include "opcua/types/status.h"

#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace opcua {
class ClientSession;

// The one server-side OPC UA subscription a ClientSession keeps, shared by
// every caller of ClientSession::CreateSubscription. Created lazily on the
// first AddItems; a background Publish loop then fans each incoming
// notification out to the view that owns the monitored item it belongs to.
//
// Callers never hold this object: each gets its own `View`
// (a MonitoredItemSubscription) with a private notification queue. That
// separation is load-bearing rather than cosmetic. An aggregating proxy runs
// several independent consumers over one downstream session — the event tap
// plus one pump per client subscription — and with a single shared ReadNext
// queue whichever consumer happened to be waiting drained notifications
// belonging to another. The event tap, which waits permanently, therefore
// swallowed every aggregated data change and dropped it on the floor (it
// forwards only events), so proxy clients saw no live values at all.
// Per-view queues also keep one consumer's Close from tearing down another's
// stream.
class ClientSubscription
    : public std::enable_shared_from_this<ClientSubscription> {
 public:
  static std::shared_ptr<ClientSubscription> Create(ClientSession& session);

  // Sets the W3C traceparent stamped on this subscription's CreateSubscription
  // and CreateMonitoredItems requests, so the server's subscription spans
  // continue the caller's trace. Empty (the default) sends no traceparent and
  // is exactly the previous behaviour.
  //
  // A session reuses one subscription across callers, so this is last-writer-
  // wins: attribution is exact when a caller owns its session (an E2E probe),
  // approximate when several share one.
  void SetTraceParent(std::string trace_parent);

  ClientSubscription(const ClientSubscription&) = delete;
  ClientSubscription& operator=(const ClientSubscription&) = delete;
  ~ClientSubscription();

  // Hands out an independent consumer of this shared subscription. The view
  // keeps the subscription alive for as long as it lives; destroying it
  // removes the items it added.
  [[nodiscard]] std::unique_ptr<MonitoredItemSubscription> CreateView();

  // Closes every outstanding view with `status`, so consumers waiting in
  // ReadNext observe the loss instead of waiting forever. Called when the
  // session drops the subscription.
  void CloseAllViews(Status status);

 private:
  // One consumer's notification queue. Held by shared_ptr so a monitored
  // item's notification callback reaches its owner directly, with no lookup
  // and without keeping the View itself alive.
  struct ViewState {
    explicit ViewState(const AnyExecutor& executor);

    std::mutex mutex;
    std::deque<ItemNotification> pending_notifications;
    Status close_status = StatusCode::Bad_NoCommunication;
    bool closed = false;
    // Wakes a ReadNext that is waiting for notifications: PushNotification and
    // the close path cancel it instead of ReadNext busy-polling. Held at
    // time_point::max() while idle; a cancel resumes the waiter, which
    // re-checks the queue.
    boost::asio::steady_timer wakeup_timer;
  };

  class View;

  explicit ClientSubscription(ClientSession& session);

  // View-facing operations. `view` identifies the calling consumer so items
  // and notifications stay attributed to it.
  [[nodiscard]] Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::shared_ptr<ViewState> view,
      std::vector<MonitoredItemCreateRequest> requests);
  [[nodiscard]] Awaitable<std::vector<Status>> RemoveItems(
      const std::shared_ptr<ViewState>& view,
      std::span<const MonitoredItemId> item_ids);
  void CloseView(const std::shared_ptr<ViewState>& view, Status status);

  void EnsureCreated();
  void StartPublishLoop();
  void FlushPendingSubscriptions();
  void SpawnCreateMonitoredItem(std::shared_ptr<ViewState> view,
                                std::uint32_t local_id,
                                ReadValueId read_value_id,
                                MonitoringParameters params,
                                std::uint32_t client_handle);
  // Drops `local_id` from the bookkeeping and, if the create already
  // completed, deletes the item on the server.
  void RemoveItem(std::uint32_t local_id);

  static void PushNotification(const std::shared_ptr<ViewState>& view,
                               ItemNotification notification);
  static CoStatusOr<std::vector<ItemNotification>> ReadNext(
      std::shared_ptr<ViewState> view,
      std::size_t max_count);

  struct PendingSubscription {
    std::shared_ptr<ViewState> view;
    std::uint32_t local_id = 0;
    ReadValueId read_value_id;
    MonitoringParameters params;
    std::uint32_t client_handle = 0;
  };

  // One monitored item, keyed by the local id handed back to the view as its
  // MonitoredItemId. `server_id` stays 0 until the create completes.
  struct ItemRecord {
    std::weak_ptr<ViewState> view;
    MonitoredItemId server_id = 0;
  };

  ClientSession& session_;
  std::string trace_parent_;
  std::unique_ptr<ClientProtocolSubscription> impl_;
  bool is_creating_ = false;
  bool publish_loop_running_ = false;

  // Guards the bookkeeping below, which several consumers reach concurrently
  // (each pump runs on its own executor). Never held across a co_await, and
  // never held while taking a ViewState::mutex.
  std::mutex mutex_;
  std::uint32_t next_local_id_ = 1;
  std::unordered_map<std::uint32_t, ItemRecord> items_by_local_id_;
  std::vector<PendingSubscription> pending_subscriptions_;
  std::vector<std::weak_ptr<ViewState>> views_;
};

}  // namespace opcua
