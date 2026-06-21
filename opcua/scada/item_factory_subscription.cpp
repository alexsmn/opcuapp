#include "opcua/scada/item_factory_subscription.h"

#include "opcua/base/boost_log.h"
#include "opcua/endpoint_core.h"
#include "opcua/scada/attribute_ids.h"

#include <algorithm>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <deque>
#include <mutex>
#include <utility>

namespace opcua {
namespace scada {
namespace {

// Parses the EventFilter select-clause browse paths from a monitored item's
// requested monitoring filter, falling back to the default BaseEventType fields
// when no usable filter is present. The resulting paths drive event-field
// projection so events leave the boundary as a standard `EventFieldList`.
std::vector<std::vector<std::string>> ParseItemEventFieldPaths(
    const std::optional<MonitoringFilter>& filter) {
  const auto* raw_filter =
      filter ? std::get_if<boost::json::value>(&*filter) : nullptr;
  if (!raw_filter)
    return NormalizeEventFieldPaths({});
  return ParseEventFilterFieldPaths(*raw_filter);
}

// Subscription that creates each monitored item through a `MonitoredItemFactory`
// and fans the items' callbacks into a single notification stream consumed via
// `ReadNext`.
class ItemFactorySubscription final : public MonitoredItemSubscription {
 public:
  ItemFactorySubscription(MonitoredItemFactory factory,
                          MonitoredItemSubscriptionOptions options)
      : state_{std::make_shared<State>(std::move(factory), options)} {}

  ~ItemFactorySubscription() override { Close(StatusCode::Bad_Disconnected); }

  Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests) override {
    std::vector<MonitoredItemCreateResult> results;
    results.reserve(requests.size());

    for (auto& request : requests) {
      results.emplace_back(AddItem(std::move(request)));
    }

    co_return results;
  }

  Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids) override {
    std::vector<Status> results;
    results.reserve(item_ids.size());

    std::lock_guard lock{state_->mutex};
    for (auto item_id : item_ids) {
      if (item_id == 0 || item_id > state_->items.size() ||
          !state_->items[item_id - 1].monitored_item) {
        results.emplace_back(StatusCode::Bad_MonitoredItemIdInvalid);
        continue;
      }

      state_->items[item_id - 1].monitored_item.reset();
      results.emplace_back(StatusCode::Good);
    }

    co_return results;
  }

  Awaitable<StatusOr<std::vector<ItemNotification>>> ReadNext(
      std::size_t max_count) override {
    if (max_count == 0) {
      co_return std::vector<ItemNotification>{};
    }

    for (;;) {
      std::vector<ItemNotification> result;

      {
        std::lock_guard lock{state_->mutex};
        if (state_->closed) {
          co_return state_->close_status;
        }

        const std::size_t count = std::min(
            {max_count, state_->options.max_batch_size, state_->pending.size()});
        if (count != 0) {
          result.reserve(count);
          for (std::size_t i = 0; i < count; ++i) {
            result.emplace_back(std::move(state_->pending.front()));
            state_->pending.pop_front();
          }
          co_return result;
        }

        if (state_->read_waiter) {
          co_return Status{StatusCode::Bad_ObjectIsBusy};
        }
      }

      auto executor = co_await boost::asio::this_coro::executor;
      auto timer = std::make_shared<boost::asio::steady_timer>(executor);
      timer->expires_at((boost::asio::steady_timer::time_point::max)());
      {
        std::lock_guard lock{state_->mutex};
        if (state_->closed) {
          co_return state_->close_status;
        }
        if (!state_->pending.empty()) {
          continue;
        }
        state_->read_waiter = timer;
      }

      boost::system::error_code error;
      co_await timer->async_wait(
          boost::asio::redirect_error(boost::asio::use_awaitable, error));

      {
        std::lock_guard lock{state_->mutex};
        if (state_->read_waiter == timer) {
          state_->read_waiter.reset();
        }
      }
    }
  }

  void Close(Status status) override {
    std::lock_guard lock{state_->mutex};
    if (state_->closed) {
      return;
    }

    state_->closed = true;
    state_->close_status = std::move(status);
    state_->items.clear();
    state_->pending.clear();
    if (state_->read_waiter) {
      state_->read_waiter->cancel();
      state_->read_waiter.reset();
    }
  }

 private:
  struct Item {
    std::shared_ptr<MonitoredItem> monitored_item;
    std::uint32_t client_handle = 0;
  };

  struct State {
    State(MonitoredItemFactory factory,
          MonitoredItemSubscriptionOptions options)
        : factory{std::move(factory)}, options{options} {}

    MonitoredItemFactory factory;
    MonitoredItemSubscriptionOptions options;
    std::mutex mutex;
    std::vector<Item> items;
    std::deque<ItemNotification> pending;
    std::shared_ptr<boost::asio::steady_timer> read_waiter;
    bool overflow_reported = false;
    bool closed = false;
    Status close_status = StatusCode::Bad_Disconnected;
  };

  static void PushNotification(std::weak_ptr<State> weak_state,
                               ItemNotification notification) {
    auto state = weak_state.lock();
    if (!state) {
      return;
    }

    std::lock_guard lock{state->mutex};
    if (state->closed) {
      return;
    }

    if (state->pending.size() >= state->options.max_pending_notifications) {
      // Queue full: drop the notification. Overflow is not delivered as its own
      // wire notification (the legacy consumer ignored overflow anyway); we only
      // log the first occurrence so the dropped-notifications signal survives.
      if (!state->overflow_reported) {
        state->overflow_reported = true;
        BOOST_LOG_TRIVIAL(warning)
            << "MonitoredItem notification queue full; dropping notifications";
      }
      return;
    }

    state->pending.emplace_back(std::move(notification));
    if (state->read_waiter) {
      state->read_waiter->cancel();
      state->read_waiter.reset();
    }
  }

  MonitoredItemCreateResult AddItem(MonitoredItemCreateRequest request) {
    // The wire request carries the per-item client_handle inside its
    // requested_parameters.
    const auto client_handle = request.requested_parameters.client_handle;
    {
      std::lock_guard lock{state_->mutex};
      if (state_->closed) {
        return {.status = state_->close_status};
      }
    }

    auto monitored_item =
        state_->factory(request.item_to_monitor, request.requested_parameters);
    if (!monitored_item) {
      const auto status =
          request.item_to_monitor.attribute_id == AttributeId::Value ||
                  request.item_to_monitor.attribute_id ==
                      AttributeId::EventNotifier
              ? StatusCode::Bad_WrongNodeId
              : StatusCode::Bad_WrongAttributeId;
      return {.status = status};
    }

    MonitoredItemId item_id = 0;
    {
      std::lock_guard lock{state_->mutex};
      item_id = static_cast<MonitoredItemId>(state_->items.size() + 1);
      state_->items.push_back({.monitored_item = monitored_item,
                               .client_handle = client_handle});
    }

    std::weak_ptr<State> weak_state = state_;
    if (request.item_to_monitor.attribute_id == AttributeId::EventNotifier) {
      // Project the event onto the item's EventFilter select clauses right here,
      // where the event meets its field paths, so only the standard wire
      // `EventFieldList` leaves the boundary (no std::any crosses it).
      auto field_paths =
          ParseItemEventFieldPaths(request.requested_parameters.filter);
      monitored_item->Subscribe(EventHandler{
          [weak_state, client_handle, field_paths = std::move(field_paths)](
              const Status& /*status*/, const std::any& event) {
            PushNotification(
                weak_state,
                opcua::EventFieldList{
                    .client_handle = client_handle,
                    .event_fields = ProjectEventFields(field_paths, event)});
          }});
    } else {
      monitored_item->Subscribe(DataChangeHandler{
          [weak_state, client_handle](const DataValue& value) {
            PushNotification(weak_state,
                             opcua::MonitoredItemNotification{
                                 .client_handle = client_handle,
                                 .value = value});
          }});
    }

    return {.status = StatusCode::Good, .monitored_item_id = item_id};
  }

  const std::shared_ptr<State> state_;
};

}  // namespace

StatusOr<std::unique_ptr<MonitoredItemSubscription>> MakeItemFactorySubscription(
    MonitoredItemFactory factory,
    MonitoredItemSubscriptionOptions options) {
  return std::unique_ptr<MonitoredItemSubscription>{
      std::make_unique<ItemFactorySubscription>(std::move(factory), options)};
}

}  // namespace scada
}  // namespace opcua (vendored)
