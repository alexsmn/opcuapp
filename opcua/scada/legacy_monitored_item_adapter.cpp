#include "opcua/scada/legacy_monitored_item_adapter.h"

#include "opcua/base/awaitable.h"
#include "opcua/scada/date_time.h"
#include "opcua/scada/monitored_item_subscription_pump.h"

#include <array>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opcua::scada {

struct LegacyMonitoredItemAdapter::ItemState {
  ItemState(ReadValueId value_id, MonitoringParameters params)
      : value_id{std::move(value_id)}, params{std::move(params)} {}

  ReadValueId value_id;
  MonitoringParameters params;
  std::mutex mutex;
  DataChangeHandler data_change_handler;
  EventHandler event_handler;
  MonitoredItemId item_id = 0;
  std::uint32_t client_handle = 0;
  bool add_requested = false;
  bool closed = false;
};

struct LegacyMonitoredItemAdapter::State {
  State(AnyExecutor executor,
        MonitoredItemService& service,
        MonitoredItemSubscriptionOptions options)
      : executor{std::move(executor)},
        service{service},
        options{std::move(options)} {}

  AnyExecutor executor;
  MonitoredItemService& service;
  MonitoredItemSubscriptionOptions options;
  std::mutex mutex;
  std::unique_ptr<MonitoredItemSubscriptionPump> pump;
  std::unordered_map<std::uint32_t, std::weak_ptr<ItemState>>
      items_by_client_handle;
  std::uint32_t next_client_handle = 1;
  bool closed = false;
};

class LegacyMonitoredItemAdapter::SubscriptionBackedMonitoredItem final
    : public MonitoredItem {
 public:
  SubscriptionBackedMonitoredItem(std::shared_ptr<State> state,
                                  ReadValueId value_id,
                                  MonitoringParameters params)
      : state_{std::move(state)},
        item_state_{std::make_shared<ItemState>(std::move(value_id),
                                                std::move(params))} {}

  ~SubscriptionBackedMonitoredItem() override { Close(); }

  void Subscribe(MonitoredItemHandler handler) override {
    bool add_item = false;
    {
      std::lock_guard lock{item_state_->mutex};
      if (auto* data_change_handler =
              std::get_if<DataChangeHandler>(&handler)) {
        item_state_->data_change_handler = std::move(*data_change_handler);
      } else if (auto* event_handler = std::get_if<EventHandler>(&handler)) {
        item_state_->event_handler = std::move(*event_handler);
      } else {
        return;
      }
      add_item = !item_state_->closed && !item_state_->add_requested;
    }

    if (add_item)
      AddItem(state_, item_state_);
  }

 private:
  void Close() {
    std::uint32_t client_handle = 0;
    MonitoredItemId item_id = 0;
    {
      std::lock_guard lock{item_state_->mutex};
      if (item_state_->closed)
        return;
      item_state_->closed = true;
      client_handle = item_state_->client_handle;
      item_id = item_state_->item_id;
    }

    if (client_handle != 0)
      RemoveItem(state_, client_handle, item_id);
  }

  const std::shared_ptr<State> state_;
  const std::shared_ptr<ItemState> item_state_;
};

LegacyMonitoredItemAdapter::LegacyMonitoredItemAdapter(
    AnyExecutor executor,
    MonitoredItemService& service,
    MonitoredItemSubscriptionOptions options)
    : state_{std::make_shared<State>(std::move(executor),
                                     service,
                                     std::move(options))} {}

LegacyMonitoredItemAdapter::~LegacyMonitoredItemAdapter() {
  Close(StatusCode::Bad_Disconnected);
}

std::shared_ptr<MonitoredItem> LegacyMonitoredItemAdapter::CreateMonitoredItem(
    ReadValueId value_id,
    MonitoringParameters params) {
  return std::make_shared<SubscriptionBackedMonitoredItem>(
      state_, std::move(value_id), std::move(params));
}

void LegacyMonitoredItemAdapter::Close(Status status) {
  CloseAll(state_, std::move(status));
}

void LegacyMonitoredItemAdapter::AddItem(
    std::shared_ptr<State> state,
    std::shared_ptr<ItemState> item_state) {
  {
    std::lock_guard item_lock{item_state->mutex};
    if (item_state->closed || item_state->add_requested)
      return;
    item_state->add_requested = true;
  }

  CoSpawn(state->executor,
          [state = std::move(state),
           item_state = std::move(item_state)]() -> Awaitable<void> {
            MonitoredItemSubscriptionPump* pump = nullptr;
            std::uint32_t client_handle = 0;
            Status start_status = StatusCode::Good;
            {
              std::lock_guard lock{state->mutex};
              if (state->closed) {
                CloseItem(item_state);
                co_return;
              }

              if (!state->pump) {
                state->pump = std::make_unique<MonitoredItemSubscriptionPump>(
                    state->executor, state->service, state->options,
                    [weak_state = std::weak_ptr<State>{state}](
                        std::vector<MonitoredItemNotification> notifications) {
                      OnNotifications(std::move(weak_state),
                                      std::move(notifications));
                    },
                    [weak_state = std::weak_ptr<State>{state}](Status status) {
                      CloseAll(std::move(weak_state), std::move(status));
                    });
                start_status = state->pump->Start();
              }

              if (!start_status) {
                state->closed = true;
                CloseItem(item_state);
                co_return;
              }

              client_handle = state->next_client_handle++;
              state->items_by_client_handle[client_handle] = item_state;
              pump = state->pump.get();
            }

            std::vector<MonitoredItemCreateRequest> requests;
            requests.emplace_back(MonitoredItemCreateRequest{
                .item_to_monitor = item_state->value_id,
                .parameters = item_state->params,
                .client_handle = client_handle});
            std::vector<MonitoredItemCreateResult> results =
                co_await pump->AddItems(std::move(requests));
            const Status result_status = results.empty()
                                             ? Status{StatusCode::Bad}
                                             : results.front().status;
            if (!result_status) {
              EraseItemMapping(state, client_handle);
              // Surface the add failure to the subscriber (e.g.
              // Bad_WrongNodeId) before closing, matching the legacy
              // synchronous behavior.
              DataChangeHandler data_change_handler;
              EventHandler event_handler;
              {
                std::lock_guard item_lock{item_state->mutex};
                if (!item_state->closed) {
                  data_change_handler = item_state->data_change_handler;
                  event_handler = item_state->event_handler;
                }
              }
              if (data_change_handler)
                data_change_handler(
                    DataValue{result_status.code(), DateTime::Now()});
              else if (event_handler)
                event_handler(result_status, {});
              CloseItem(item_state);
              co_return;
            }

            bool remove_created_item = false;
            MonitoredItemId item_id = results.front().item_id;
            {
              std::lock_guard item_lock{item_state->mutex};
              if (item_state->closed) {
                remove_created_item = true;
              } else {
                item_state->client_handle = client_handle;
                item_state->item_id = item_id;
              }
            }

            if (remove_created_item)
              RemoveItem(state, client_handle, item_id);
          });
}

void LegacyMonitoredItemAdapter::OnNotifications(
    std::weak_ptr<State> weak_state,
    std::vector<MonitoredItemNotification> notifications) {
  auto state = weak_state.lock();
  if (!state)
    return;

  auto resolve_item =
      [&state](std::uint32_t client_handle) -> std::shared_ptr<ItemState> {
    std::lock_guard lock{state->mutex};
    auto it = state->items_by_client_handle.find(client_handle);
    if (it == state->items_by_client_handle.end())
      return nullptr;
    return it->second.lock();
  };

  for (const auto& notification : notifications) {
    if (const auto* data_change =
            std::get_if<DataChangeNotification>(&notification)) {
      auto item_state = resolve_item(data_change->client_handle);
      if (!item_state)
        continue;
      DataChangeHandler handler;
      {
        std::lock_guard item_lock{item_state->mutex};
        if (item_state->closed)
          continue;
        handler = item_state->data_change_handler;
      }
      if (handler)
        handler(data_change->value);
    } else if (const auto* event =
                   std::get_if<EventNotification>(&notification)) {
      auto item_state = resolve_item(event->client_handle);
      if (!item_state)
        continue;
      EventHandler handler;
      {
        std::lock_guard item_lock{item_state->mutex};
        if (item_state->closed)
          continue;
        handler = item_state->event_handler;
      }
      if (handler)
        handler(event->status, event->event);
    } else if (const auto* item_status =
                   std::get_if<ItemStatusNotification>(&notification)) {
      auto item_state = resolve_item(item_status->client_handle);
      if (!item_state)
        continue;
      DataChangeHandler data_change_handler;
      EventHandler event_handler;
      {
        std::lock_guard item_lock{item_state->mutex};
        if (item_state->closed)
          continue;
        data_change_handler = item_state->data_change_handler;
        event_handler = item_state->event_handler;
      }
      if (data_change_handler)
        data_change_handler(
            DataValue{item_status->status.code(), DateTime::Now()});
      else if (event_handler)
        event_handler(item_status->status, {});
    }
    // OverflowNotification carries no item id; nothing to deliver.
  }
}

void LegacyMonitoredItemAdapter::RemoveItem(std::shared_ptr<State> state,
                                            std::uint32_t client_handle,
                                            MonitoredItemId item_id) {
  {
    std::lock_guard lock{state->mutex};
    state->items_by_client_handle.erase(client_handle);
    if (state->closed || !state->pump || item_id == 0)
      return;
  }

  CoSpawn(state->executor,
          [state = std::move(state), item_id]() -> Awaitable<void> {
            MonitoredItemSubscriptionPump* pump = nullptr;
            {
              std::lock_guard lock{state->mutex};
              if (state->closed || !state->pump)
                co_return;
              pump = state->pump.get();
            }
            const std::array item_ids{item_id};
            co_await pump->RemoveItems(item_ids);
          });
}

void LegacyMonitoredItemAdapter::EraseItemMapping(std::shared_ptr<State> state,
                                                  std::uint32_t client_handle) {
  std::lock_guard lock{state->mutex};
  state->items_by_client_handle.erase(client_handle);
}

void LegacyMonitoredItemAdapter::CloseItem(
    std::shared_ptr<ItemState> item_state) {
  std::lock_guard item_lock{item_state->mutex};
  item_state->closed = true;
}

void LegacyMonitoredItemAdapter::CloseAll(std::weak_ptr<State> weak_state,
                                          Status status) {
  auto state = weak_state.lock();
  if (state)
    CloseAll(std::move(state), std::move(status));
}

void LegacyMonitoredItemAdapter::CloseAll(std::shared_ptr<State> state,
                                          Status status) {
  std::vector<std::shared_ptr<ItemState>> item_states;
  MonitoredItemSubscriptionPump* pump = nullptr;
  {
    std::lock_guard lock{state->mutex};
    if (state->closed)
      return;
    state->closed = true;
    item_states.reserve(state->items_by_client_handle.size());
    for (auto& [client_handle, weak_item_state] :
         state->items_by_client_handle) {
      if (auto item_state = weak_item_state.lock())
        item_states.emplace_back(std::move(item_state));
    }
    state->items_by_client_handle.clear();
    pump = state->pump.get();
  }

  for (auto& item_state : item_states) {
    CloseItem(std::move(item_state));
  }

  if (pump)
    pump->Close(std::move(status));
}

}  // namespace opcua::scada
