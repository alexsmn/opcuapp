#include "opcua/client/client_subscription.h"

#include "opcua/client/client_session.h"
#include "opcua/types/co_result.h"
#include "opcua/types/data_value.h"
#include "opcua/types/date_time.h"

#include <algorithm>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <utility>
#include <variant>

namespace opcua {

namespace {

constexpr double kDefaultPublishingIntervalMs = 500.0;
constexpr std::uint32_t kDefaultLifetimeCount = 3000;
constexpr std::uint32_t kDefaultMaxKeepAliveCount = 10000;

}  // namespace

// One consumer's handle on the shared subscription. Everything it does is
// scoped to its own ViewState, so two views over the same session never see
// each other's items or notifications.
class ClientSubscription::View final : public MonitoredItemSubscription {
 public:
  View(std::shared_ptr<ClientSubscription> owner,
       std::shared_ptr<ViewState> state)
      : owner_{std::move(owner)}, state_{std::move(state)} {}

  ~View() override {
    // A view that goes away without an explicit Close still owns items on the
    // server; releasing them here keeps a long-lived downstream session from
    // accumulating one leaked monitored item per client subscription.
    owner_->CloseView(state_, StatusCode::Bad_NoSubscription);
  }

  View(const View&) = delete;
  View& operator=(const View&) = delete;

  Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests) override {
    co_return co_await owner_->AddItems(state_, std::move(requests));
  }

  Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids) override {
    co_return co_await owner_->RemoveItems(state_, item_ids);
  }

  CoStatusOr<std::vector<ItemNotification>> ReadNext(
      std::size_t max_count) override {
    co_return co_await ClientSubscription::ReadNext(state_, max_count);
  }

  void Close(Status status) override {
    owner_->CloseView(state_, std::move(status));
  }

 private:
  const std::shared_ptr<ClientSubscription> owner_;
  const std::shared_ptr<ViewState> state_;
};

ClientSubscription::ViewState::ViewState(const AnyExecutor& executor)
    : wakeup_timer{executor} {
  wakeup_timer.expires_at(boost::asio::steady_timer::time_point::max());
}

// static
std::shared_ptr<ClientSubscription> ClientSubscription::Create(
    ClientSession& session) {
  return std::shared_ptr<ClientSubscription>(new ClientSubscription(session));
}

ClientSubscription::ClientSubscription(ClientSession& session)
    : session_{session} {}

ClientSubscription::~ClientSubscription() = default;

void ClientSubscription::SetTraceParent(std::string trace_parent) {
  trace_parent_ = std::move(trace_parent);
}

std::unique_ptr<MonitoredItemSubscription> ClientSubscription::CreateView() {
  // A fresh state per call is the whole point (see the class comment). Handing
  // the same ViewState to every view makes the first ReadNext drain the
  // notifications addressed to all the others, which then discard them as
  // unknown client handles — so only the first caller of CreateSubscription
  // ever sees data.
  auto state = std::make_shared<ViewState>(session_.any_executor());
  {
    std::lock_guard lock{mutex_};
    views_.push_back(state);
  }
  return std::make_unique<View>(shared_from_this(), std::move(state));
}

void ClientSubscription::CloseAllViews(Status status) {
  std::vector<std::shared_ptr<ViewState>> views;
  {
    std::lock_guard lock{mutex_};
    views.reserve(views_.size());
    for (const auto& weak_view : views_) {
      if (auto view = weak_view.lock()) {
        views.push_back(std::move(view));
      }
    }
  }
  for (const auto& view : views) {
    CloseView(view, status);
  }
}

void ClientSubscription::EnsureCreated() {
  if (impl_ || is_creating_) {
    return;
  }
  is_creating_ = true;
  impl_ = std::make_unique<ClientProtocolSubscription>(session_.channel());
  CoSpawn(
      session_.any_executor(), weak_from_this(),
      [](std::shared_ptr<ClientSubscription> self) mutable -> Awaitable<void> {
        SubscriptionParameters params{
            .publishing_interval_ms = kDefaultPublishingIntervalMs,
            .lifetime_count = kDefaultLifetimeCount,
            .max_keep_alive_count = kDefaultMaxKeepAliveCount,
            .max_notifications_per_publish = 0,
            .publishing_enabled = true,
            .priority = 0,
        };
        const auto status =
            co_await self->impl_->Create(params, self->trace_parent_);
        self->is_creating_ = false;
        if (status.bad()) {
          self->impl_.reset();
          std::lock_guard lock{self->mutex_};
          self->pending_subscriptions_.clear();
          co_return;
        }
        self->FlushPendingSubscriptions();
        self->StartPublishLoop();
      });
}

void ClientSubscription::StartPublishLoop() {
  if (publish_loop_running_ || !impl_) {
    return;
  }
  publish_loop_running_ = true;
  CoSpawn(
      session_.any_executor(), weak_from_this(),
      [](std::shared_ptr<ClientSubscription> self) mutable -> Awaitable<void> {
        for (;;) {
          if (!self->impl_ || !self->session_.is_connected()) {
            break;
          }
          const auto status = co_await self->impl_->Publish();
          if (status.bad()) {
            break;
          }
        }
        self->publish_loop_running_ = false;
      });
}

void ClientSubscription::FlushPendingSubscriptions() {
  std::vector<PendingSubscription> pending;
  {
    std::lock_guard lock{mutex_};
    pending = std::move(pending_subscriptions_);
    pending_subscriptions_.clear();
  }
  for (auto& item : pending) {
    SpawnCreateMonitoredItem(std::move(item.view), item.local_id,
                             std::move(item.read_value_id),
                             std::move(item.params), item.client_handle);
  }
}

void ClientSubscription::SpawnCreateMonitoredItem(
    std::shared_ptr<ViewState> view,
    std::uint32_t local_id,
    ReadValueId read_value_id,
    MonitoringParameters params,
    std::uint32_t client_handle) {
  CoSpawn(
      session_.any_executor(), weak_from_this(),
      [view = std::move(view), local_id,
       read_value_id = std::move(read_value_id), params = std::move(params),
       client_handle](
          std::shared_ptr<ClientSubscription> self) mutable -> Awaitable<void> {
        if (!self->impl_) {
          co_return;
        }
        // client_handle is assigned by the protocol subscription; the caller's
        // handle is the one echoed back on this view's notifications, and is
        // only meaningful within this view.
        params.client_handle = 0;
        std::string trace_parent = self->trace_parent_;
        // The notification callbacks capture the owning view weakly: a view
        // that goes away must not keep its queue alive, and its items are
        // deleted server-side by CloseView anyway.
        std::weak_ptr<ViewState> weak_view = view;
        auto result = co_await self->impl_->CreateMonitoredItem(
            std::move(read_value_id), std::move(params),
            [weak_view, client_handle](DataValue value) {
              auto view = weak_view.lock();
              if (!view)
                return;
              PushNotification(view, MonitoredItemNotification{
                                         .client_handle = client_handle,
                                         .value = std::move(value)});
            },
            [weak_view, client_handle](std::vector<Variant> event_fields) {
              auto view = weak_view.lock();
              if (!view)
                return;
              PushNotification(
                  view,
                  EventFieldList{.client_handle = client_handle,
                                 .event_fields = std::move(event_fields)});
            },
            std::move(trace_parent));
        if (result.ok()) {
          bool still_owned = false;
          {
            std::lock_guard lock{self->mutex_};
            if (auto it = self->items_by_local_id_.find(local_id);
                it != self->items_by_local_id_.end()) {
              it->second.server_id = result->monitored_item_id;
              still_owned = true;
            }
          }
          // The view removed the item (or went away) while the create was in
          // flight; undo it rather than leak it on the server.
          if (!still_owned) {
            co_await self->impl_->DeleteMonitoredItem(
                result->monitored_item_id);
          }
          co_return;
        }
        // The record stays: AddItems already handed this local id to the view
        // as its MonitoredItemId, so RemoveItems on it must keep working. It
        // has no server id, so removing it never reaches the wire. The refusal
        // itself is reported as a notification.
        if (auto view = weak_view.lock()) {
          DataValue value;
          value.status_code = result.status().code();
          PushNotification(
              view, MonitoredItemNotification{.client_handle = client_handle,
                                              .value = std::move(value)});
        }
      });
}

Awaitable<std::vector<MonitoredItemCreateResult>> ClientSubscription::AddItems(
    std::shared_ptr<ViewState> view,
    std::vector<MonitoredItemCreateRequest> requests) {
  EnsureCreated();

  std::vector<MonitoredItemCreateResult> results;
  results.reserve(requests.size());
  for (auto& request : requests) {
    const std::uint32_t client_handle =
        request.requested_parameters.client_handle;
    const double revised_sampling_interval_ms =
        request.requested_parameters.sampling_interval_ms;
    const UInt32 revised_queue_size =
        std::max<UInt32>(1, request.requested_parameters.queue_size);
    const bool defer = is_creating_ || !impl_;

    std::uint32_t local_id = 0;
    {
      std::lock_guard lock{mutex_};
      local_id = next_local_id_++;
      items_by_local_id_.emplace(local_id, ItemRecord{.view = view});
      if (defer) {
        pending_subscriptions_.push_back(PendingSubscription{
            .view = view,
            .local_id = local_id,
            .read_value_id = std::move(request.item_to_monitor),
            .params = std::move(request.requested_parameters),
            .client_handle = client_handle,
        });
      }
    }
    if (!defer) {
      SpawnCreateMonitoredItem(
          view, local_id, std::move(request.item_to_monitor),
          std::move(request.requested_parameters), client_handle);
    }
    results.push_back(
        {.status = StatusCode::Good,
         .monitored_item_id = local_id,
         .revised_sampling_interval_ms = revised_sampling_interval_ms,
         .revised_queue_size = revised_queue_size});
  }

  co_return results;
}

Awaitable<std::vector<Status>> ClientSubscription::RemoveItems(
    const std::shared_ptr<ViewState>& view,
    std::span<const MonitoredItemId> item_ids) {
  std::vector<Status> results;
  results.reserve(item_ids.size());

  for (const MonitoredItemId local_id : item_ids) {
    // Local ids are unique across the whole subscription, so scope the removal
    // to this view rather than letting one consumer drop another's item.
    bool owned = false;
    {
      std::lock_guard lock{mutex_};
      if (auto it = items_by_local_id_.find(local_id);
          it != items_by_local_id_.end()) {
        owned = it->second.view.lock() == view;
      }
    }
    if (!owned) {
      results.push_back(StatusCode::Bad_MonitoredItemIdInvalid);
      continue;
    }
    results.push_back(StatusCode::Good);
    RemoveItem(local_id);
  }

  co_return results;
}

void ClientSubscription::RemoveItem(std::uint32_t local_id) {
  MonitoredItemId server_id = 0;
  {
    std::lock_guard lock{mutex_};
    std::erase_if(pending_subscriptions_,
                  [local_id](const PendingSubscription& pending) {
                    return pending.local_id == local_id;
                  });
    if (auto it = items_by_local_id_.find(local_id);
        it != items_by_local_id_.end()) {
      server_id = it->second.server_id;
      items_by_local_id_.erase(it);
    }
  }
  // Still waiting on its CreateMonitoredItem; that coroutine sees the record
  // is gone and deletes the item itself.
  if (server_id == 0) {
    return;
  }

  CoSpawn(session_.any_executor(), weak_from_this(),
          [server_id](std::shared_ptr<ClientSubscription> self) mutable
              -> Awaitable<void> {
            if (!self->impl_) {
              co_return;
            }
            co_await self->impl_->DeleteMonitoredItem(server_id);
          });
}

void ClientSubscription::CloseView(const std::shared_ptr<ViewState>& view,
                                   Status status) {
  {
    std::lock_guard lock{view->mutex};
    if (view->closed)
      return;
    view->closed = true;
    view->close_status = std::move(status);
    view->pending_notifications.clear();
  }
  view->wakeup_timer.cancel();  // wake a waiting ReadNext to observe the close

  std::vector<std::uint32_t> local_ids;
  {
    std::lock_guard lock{mutex_};
    for (const auto& [local_id, record] : items_by_local_id_) {
      if (record.view.lock() == view) {
        local_ids.push_back(local_id);
      }
    }
    std::erase_if(views_, [&view](const std::weak_ptr<ViewState>& weak_view) {
      auto held = weak_view.lock();
      return !held || held == view;
    });
  }
  for (const std::uint32_t local_id : local_ids) {
    RemoveItem(local_id);
  }
}

// static
CoStatusOr<std::vector<ItemNotification>> ClientSubscription::ReadNext(
    std::shared_ptr<ViewState> view,
    std::size_t max_count) {
  for (;;) {
    {
      std::lock_guard lock{view->mutex};
      if (view->closed)
        co_return view->close_status;

      if (!view->pending_notifications.empty() || max_count == 0) {
        std::vector<ItemNotification> result;
        const auto count =
            std::min(max_count, view->pending_notifications.size());
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
          result.push_back(std::move(view->pending_notifications.front()));
          view->pending_notifications.pop_front();
        }
        co_return result;
      }
    }

    // Wait until PushNotification or the close path cancels the wakeup timer,
    // then re-check the queue. The timer sits at time_point::max() so it only
    // returns when cancelled; this replaces a busy 10 ms poll and resumes the
    // waiter in the same turn the notification is delivered (so it does not
    // depend on a poll timer firing).
    boost::system::error_code error;
    co_await view->wakeup_timer.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
  }
}

// static
void ClientSubscription::PushNotification(
    const std::shared_ptr<ViewState>& view,
    ItemNotification notification) {
  {
    std::lock_guard lock{view->mutex};
    if (view->closed)
      return;
    view->pending_notifications.push_back(std::move(notification));
  }
  view->wakeup_timer.cancel();  // wake a waiting ReadNext to drain the new item
}

}  // namespace opcua
