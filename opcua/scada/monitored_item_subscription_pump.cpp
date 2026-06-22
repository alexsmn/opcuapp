#include "opcua/scada/monitored_item_subscription_pump.h"

#include "opcua/base/awaitable.h"
#include "opcua/scada/service_context.h"

#include <mutex>

namespace opcua {
namespace scada {

struct MonitoredItemSubscriptionPump::State {
  std::mutex mutex;
  MonitoredItemSubscriptionOptions options;
  NotificationBatchHandler notification_batch_handler;
  ErrorHandler error_handler;
  bool closed = false;
  std::unique_ptr<MonitoredItemSubscription> subscription;
};

MonitoredItemSubscriptionPump::MonitoredItemSubscriptionPump(
    AnyExecutor executor,
    ServiceCallbacks::CreateSubscriptionCallback create_subscription,
    MonitoredItemSubscriptionOptions options,
    NotificationBatchHandler notification_batch_handler,
    ErrorHandler error_handler)
    : executor_{std::move(executor)},
      create_subscription_{std::move(create_subscription)},
      options_{options},
      notification_batch_handler_{std::move(notification_batch_handler)},
      error_handler_{std::move(error_handler)} {}

MonitoredItemSubscriptionPump::~MonitoredItemSubscriptionPump() {
  Close(StatusCode::Bad_Disconnected);
}

Status MonitoredItemSubscriptionPump::Start() {
  if (state_) {
    return StatusCode::Good;
  }

  StatusOr<std::unique_ptr<MonitoredItemSubscription>> subscription_result =
      create_subscription_(ServiceContext{}, options_);
  if (!subscription_result.ok()) {
    return subscription_result.status();
  }

  state_ = std::make_shared<State>();
  state_->options = options_;
  state_->notification_batch_handler = notification_batch_handler_;
  state_->error_handler = error_handler_;
  state_->subscription = std::move(*subscription_result);

  CoSpawn(executor_, [state = state_] { return ReadLoop(std::move(state)); });

  return StatusCode::Good;
}

Awaitable<std::vector<MonitoredItemCreateResult>>
MonitoredItemSubscriptionPump::AddItems(
    std::vector<MonitoredItemCreateRequest> requests) {
  std::shared_ptr<State> state = state_;
  if (!state) {
    co_return std::vector<MonitoredItemCreateResult>(
        requests.size(),
        MonitoredItemCreateResult{.status = StatusCode::Bad_Disconnected});
  }

  std::unique_ptr<MonitoredItemSubscription>* subscription = nullptr;
  {
    std::lock_guard lock{state->mutex};
    if (state->closed || !state->subscription) {
      co_return std::vector<MonitoredItemCreateResult>(
          requests.size(),
          MonitoredItemCreateResult{.status = StatusCode::Bad_Disconnected});
    }
    subscription = &state->subscription;
  }

  co_return co_await (*subscription)->AddItems(std::move(requests));
}

Awaitable<std::vector<Status>> MonitoredItemSubscriptionPump::RemoveItems(
    std::span<const MonitoredItemId> item_ids) {
  std::shared_ptr<State> state = state_;
  if (!state) {
    co_return std::vector<Status>(item_ids.size(),
                                  StatusCode::Bad_Disconnected);
  }

  std::unique_ptr<MonitoredItemSubscription>* subscription = nullptr;
  {
    std::lock_guard lock{state->mutex};
    if (state->closed || !state->subscription) {
      co_return std::vector<Status>(item_ids.size(),
                                    StatusCode::Bad_Disconnected);
    }
    subscription = &state->subscription;
  }

  co_return co_await (*subscription)->RemoveItems(item_ids);
}

void MonitoredItemSubscriptionPump::Close(Status status) {
  if (!state_) {
    return;
  }

  std::lock_guard lock{state_->mutex};
  if (state_->closed) {
    return;
  }

  state_->closed = true;
  if (state_->subscription) {
    state_->subscription->Close(std::move(status));
  }
}

Awaitable<void> MonitoredItemSubscriptionPump::ReadLoop(
    std::shared_ptr<State> state) {
  for (;;) {
    std::unique_ptr<MonitoredItemSubscription>* subscription = nullptr;
    {
      std::lock_guard lock{state->mutex};
      if (state->closed || !state->subscription) {
        co_return;
      }
      subscription = &state->subscription;
    }

    StatusOr<std::vector<ItemNotification>> notifications =
        co_await (*subscription)->ReadNext(state->options.max_batch_size);

    {
      std::lock_guard lock{state->mutex};
      if (state->closed) {
        co_return;
      }
    }

    if (!notifications.ok()) {
      state->error_handler(notifications.status());
      co_return;
    }

    state->notification_batch_handler(std::move(*notifications));
  }
}

}  // namespace scada
}  // namespace opcua
