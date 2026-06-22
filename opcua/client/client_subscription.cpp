#include "opcua/client/client_subscription.h"

#include "opcua/client/client_session.h"
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

// static
std::shared_ptr<ClientSubscription> ClientSubscription::Create(
    ClientSession& session) {
  return std::shared_ptr<ClientSubscription>(new ClientSubscription(session));
}

ClientSubscription::ClientSubscription(ClientSession& session)
    : session_{session} {}

ClientSubscription::~ClientSubscription() = default;

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
        const auto status = co_await self->impl_->Create(params);
        self->is_creating_ = false;
        if (status.bad()) {
          self->impl_.reset();
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
  auto pending = std::move(pending_subscriptions_);
  pending_subscriptions_.clear();
  for (auto& item : pending) {
    SpawnCreateMonitoredItem(item.local_id, std::move(item.read_value_id),
                             std::move(item.params), item.client_handle);
  }
}

void ClientSubscription::SpawnCreateMonitoredItem(std::uint32_t local_id,
                                                  ReadValueId read_value_id,
                                                  MonitoringParameters params,
                                                  std::uint32_t client_handle) {
  CoSpawn(
      session_.any_executor(), weak_from_this(),
      [local_id, read_value_id = std::move(read_value_id),
       params = std::move(params), client_handle](
          std::shared_ptr<ClientSubscription> self) mutable -> Awaitable<void> {
        if (!self->impl_) {
          co_return;
        }
        // client_handle is assigned by the protocol subscription.
        params.client_handle = 0;
        auto result = co_await self->impl_->CreateMonitoredItem(
            std::move(read_value_id), std::move(params),
            [weak_self = std::weak_ptr<ClientSubscription>{self},
             client_handle](DataValue value) {
              auto self = weak_self.lock();
              if (!self)
                return;
              self->PushNotification(MonitoredItemNotification{
                  .client_handle = client_handle, .value = std::move(value)});
            });
        if (result.ok()) {
          self->server_ids_by_local_id_[local_id] = result->monitored_item_id;
        } else {
          DataValue value;
          value.status_code = result.status().code();
          self->PushNotification(MonitoredItemNotification{
              .client_handle = client_handle, .value = std::move(value)});
        }
      });
}

Awaitable<std::vector<MonitoredItemCreateResult>> ClientSubscription::AddItems(
    std::vector<MonitoredItemCreateRequest> requests) {
  EnsureCreated();

  std::vector<MonitoredItemCreateResult> results;
  results.reserve(requests.size());
  for (auto& request : requests) {
    const std::uint32_t local_id = next_local_id_++;
    const std::uint32_t client_handle =
        request.requested_parameters.client_handle;
    const double revised_sampling_interval_ms =
        request.requested_parameters.sampling_interval_ms;
    const UInt32 revised_queue_size =
        std::max<UInt32>(1, request.requested_parameters.queue_size);
    if (is_creating_ || !impl_) {
      pending_subscriptions_.push_back(PendingSubscription{
          .local_id = local_id,
          .read_value_id = std::move(request.item_to_monitor),
          .params = std::move(request.requested_parameters),
          .client_handle = client_handle,
      });
    } else {
      SpawnCreateMonitoredItem(local_id, std::move(request.item_to_monitor),
                               std::move(request.requested_parameters),
                               client_handle);
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
    std::span<const MonitoredItemId> item_ids) {
  std::vector<Status> results;
  results.reserve(item_ids.size());

  for (const MonitoredItemId local_id : item_ids) {
    results.push_back(StatusCode::Good);
    std::erase_if(pending_subscriptions_,
                  [local_id](const PendingSubscription& pending) {
                    return pending.local_id == local_id;
                  });
    if (!impl_) {
      server_ids_by_local_id_.erase(local_id);
      continue;
    }
    auto it = server_ids_by_local_id_.find(local_id);
    if (it == server_ids_by_local_id_.end()) {
      continue;
    }
    const auto server_id = it->second;
    server_ids_by_local_id_.erase(it);

    CoSpawn(session_.any_executor(), weak_from_this(),
            [server_id](std::shared_ptr<ClientSubscription> self) mutable
                -> Awaitable<void> {
              if (!self->impl_) {
                co_return;
              }
              co_await self->impl_->DeleteMonitoredItem(server_id);
            });
  }

  co_return results;
}

Awaitable<StatusOr<std::vector<ItemNotification>>> ClientSubscription::ReadNext(
    std::size_t max_count) {
  for (;;) {
    {
      std::lock_guard lock{mutex_};
      if (closed_)
        co_return close_status_;

      if (!pending_notifications_.empty() || max_count == 0) {
        std::vector<ItemNotification> result;
        const auto count = std::min(max_count, pending_notifications_.size());
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
          result.push_back(std::move(pending_notifications_.front()));
          pending_notifications_.pop_front();
        }
        co_return result;
      }
    }

    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer{executor};
    timer.expires_after(std::chrono::milliseconds{10});
    boost::system::error_code error;
    co_await timer.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
  }
}

void ClientSubscription::Close(Status status) {
  std::lock_guard lock{mutex_};
  if (closed_)
    return;
  closed_ = true;
  close_status_ = std::move(status);
  pending_notifications_.clear();
}

void ClientSubscription::PushNotification(ItemNotification notification) {
  std::lock_guard lock{mutex_};
  if (closed_)
    return;
  pending_notifications_.push_back(std::move(notification));
}

}  // namespace opcua
