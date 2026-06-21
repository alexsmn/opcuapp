#include "opcua/client_subscription.h"

#include "opcua/client_session.h"
#include "opcua/client_monitored_item.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/date_time.h"

#include <algorithm>
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
  return std::shared_ptr<ClientSubscription>(
      new ClientSubscription(session));
}

ClientSubscription::ClientSubscription(ClientSession& session)
    : session_{session} {}

ClientSubscription::~ClientSubscription() = default;

std::shared_ptr<scada::MonitoredItem>
ClientSubscription::CreateMonitoredItem(
    const ReadValueId& read_value_id,
    const scada::MonitoringParameters& params) {
  const std::uint32_t local_id = next_local_id_++;
  return std::make_shared<MonitoredItem>(shared_from_this(), local_id,
                                              read_value_id, params);
}

void ClientSubscription::EnsureCreated() {
  if (impl_ || is_creating_) {
    return;
  }
  is_creating_ = true;
  impl_ = std::make_unique<ClientProtocolSubscription>(session_.channel());
  CoSpawn(session_.any_executor(), weak_from_this(),
          [](std::shared_ptr<ClientSubscription> self) mutable
              -> Awaitable<void> {
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
  CoSpawn(session_.any_executor(), weak_from_this(),
          [](std::shared_ptr<ClientSubscription> self) mutable
              -> Awaitable<void> {
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
                             std::move(item.params),
                             std::move(item.dispatch));
  }
}

void ClientSubscription::SpawnCreateMonitoredItem(
    std::uint32_t local_id,
    ReadValueId read_value_id,
    scada::MonitoringParameters params,
    scada::DataChangeHandler dispatch) {
  CoSpawn(
      session_.any_executor(), weak_from_this(),
      [local_id, read_value_id = std::move(read_value_id),
       params = std::move(params), dispatch = std::move(dispatch)](
          std::shared_ptr<ClientSubscription> self) mutable -> Awaitable<void> {
        if (!self->impl_) {
          co_return;
        }
        MonitoringParameters monitoring{
            .client_handle = 0,  // will be overwritten by impl_
            .sampling_interval_ms =
                params.sampling_interval.has_value()
                    ? params.sampling_interval->InMillisecondsF()
                    : 0.0,
            .filter = std::nullopt,
            .queue_size =
                static_cast<UInt32>(params.queue_size.value_or(1)),
            .discard_oldest = true,
        };
        auto result = co_await self->impl_->CreateMonitoredItem(
            std::move(read_value_id), std::move(monitoring),
            [dispatch](DataValue value) {
              if (dispatch) {
                dispatch(value);
              }
            });
        if (result.ok()) {
          self->server_ids_by_local_id_[local_id] = result->monitored_item_id;
        }
      });
}

void ClientSubscription::Subscribe(
    std::uint32_t local_id,
    const ReadValueId& read_value_id,
    const scada::MonitoringParameters& params,
    scada::MonitoredItemHandler handler) {
  EnsureCreated();

  auto* data_change = std::get_if<scada::DataChangeHandler>(&handler);
  scada::DataChangeHandler dispatch;
  if (data_change) {
    dispatch = *data_change;
  }
  if (is_creating_ || !impl_) {
    pending_subscriptions_.push_back(PendingSubscription{
        .local_id = local_id,
        .read_value_id = read_value_id,
        .params = params,
        .dispatch = std::move(dispatch),
    });
    return;
  }

  SpawnCreateMonitoredItem(local_id, read_value_id, params,
                           std::move(dispatch));
}

void ClientSubscription::Unsubscribe(std::uint32_t local_id) {
  std::erase_if(pending_subscriptions_,
                [local_id](const PendingSubscription& pending) {
                  return pending.local_id == local_id;
                });
  if (!impl_) {
    server_ids_by_local_id_.erase(local_id);
    return;
  }
  auto it = server_ids_by_local_id_.find(local_id);
  if (it == server_ids_by_local_id_.end()) {
    return;
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

}  // namespace opcua
