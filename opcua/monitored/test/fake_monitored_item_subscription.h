#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/monitored/monitored_item.h"
#include "opcua/services/service_callbacks.h"
#include "opcua/services/service_context.h"
#include "opcua/types/co_result.h"
#include "opcua/types/status.h"

#include <algorithm>

#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace opcua {

// Test double for a backing `MonitoredItemSubscription` — the seam a
// `ServerSubscription` binds its monitored items to (on a real server, a
// session to the address space or to a downstream tier).
//
// The subscription under test owns the fake through a `unique_ptr`, so
// everything a test needs to observe or drive lives in a separately owned
// `State` that the test keeps a `shared_ptr` to. Use `MakeCreateSubscription`
// to build the `ServiceCallbacks::CreateSubscriptionCallback` a
// `ServerSubscription` is constructed with.
//
// `ReadNext` parks — it never completes with an empty batch — so a test driver
// that polls its executor until idle (e.g. `Drain`) terminates. Pushing a
// notification wakes a parked reader through a posted task, so notifications
// reach the subscription when the executor is polled, as they do in
// production.
class FakeMonitoredItemSubscription final : public MonitoredItemSubscription {
 public:
  // One item the subscription under test bound. `request.requested_parameters
  // .client_handle` is the *backing* client handle the ServerSubscription
  // allocated for it, which is what a notification must be pushed under.
  struct AddedItem {
    MonitoredItemId item_id = 0;
    MonitoredItemCreateRequest request;
  };

  struct State {
    // Status reported for every item in an `AddItems` batch. Set to a bad
    // status to exercise the bind-failure path.
    Status add_status{StatusCode::Good};

    std::vector<AddedItem> added_items;
    std::vector<MonitoredItemId> removed_item_ids;
    bool closed = false;
    Status close_status{StatusCode::Good};

    // The backing client handle allocated for the `index`-th created item.
    UInt32 BackingClientHandle(std::size_t index) const {
      return index < added_items.size()
                 ? added_items[index].request.requested_parameters.client_handle
                 : 0;
    }

    void PushDataChange(UInt32 client_handle, DataValue value) {
      Push(MonitoredItemNotification{.client_handle = client_handle,
                                     .value = std::move(value)});
    }

    void PushEvent(UInt32 client_handle, std::vector<Variant> event_fields) {
      Push(EventFieldList{.client_handle = client_handle,
                          .event_fields = std::move(event_fields)});
    }

    void Push(ItemNotification notification) {
      notifications.push_back(std::move(notification));
      WakeReader();
    }

   private:
    friend class FakeMonitoredItemSubscription;

    void WakeReader() {
      if (!reader_waiter)
        return;
      boost::asio::post(executor, std::exchange(reader_waiter, {}));
    }

    AnyExecutor executor;
    std::deque<ItemNotification> notifications;
    // Resumes a `ReadNext` parked on an empty queue.
    std::function<void()> reader_waiter;

    MonitoredItemId next_item_id = 1;
  };

  FakeMonitoredItemSubscription(AnyExecutor executor,
                                std::shared_ptr<State> state)
      : state_{std::move(state)} {
    state_->executor = std::move(executor);
  }

  // Builds the create-subscription callback a ServerSubscription takes,
  // handing out fakes backed by `state`.
  static ServiceCallbacks::CreateSubscriptionCallback MakeCreateSubscription(
      AnyExecutor executor,
      std::shared_ptr<State> state) {
    return [executor = std::move(executor), state = std::move(state)](
               ServiceContext, MonitoredItemSubscriptionOptions)
               -> StatusOr<std::unique_ptr<MonitoredItemSubscription>> {
      return std::unique_ptr<MonitoredItemSubscription>{
          std::make_unique<FakeMonitoredItemSubscription>(executor, state)};
    };
  }

  Awaitable<std::vector<MonitoredItemCreateResult>> AddItems(
      std::vector<MonitoredItemCreateRequest> requests) override {
    std::vector<MonitoredItemCreateResult> results;
    results.reserve(requests.size());
    for (auto& request : requests) {
      if (!state_->add_status) {
        results.push_back({.status = state_->add_status});
        continue;
      }
      const MonitoredItemId item_id = state_->next_item_id++;
      results.push_back(
          {.status = StatusCode::Good,
           .monitored_item_id = item_id,
           .revised_sampling_interval_ms =
               request.requested_parameters.sampling_interval_ms,
           .revised_queue_size = request.requested_parameters.queue_size});
      state_->added_items.push_back(
          {.item_id = item_id, .request = std::move(request)});
    }
    co_return results;
  }

  Awaitable<std::vector<Status>> RemoveItems(
      std::span<const MonitoredItemId> item_ids) override {
    std::vector<Status> results;
    results.reserve(item_ids.size());
    for (const auto item_id : item_ids) {
      state_->removed_item_ids.push_back(item_id);
      results.push_back(Status{StatusCode::Good});
    }
    co_return results;
  }

  // Suspends until a notification is pushed (or the subscription closes).
  // Defined ahead of its only caller: a deduced return type cannot be used
  // before it is defined.
  template <class CompletionToken>
  static auto ParkReader(std::shared_ptr<State> state,
                         CompletionToken&& token) {
    return boost::asio::async_initiate<CompletionToken, void()>(
        [state = std::move(state)](auto handler) {
          state->reader_waiter =
              [shared_handler = std::make_shared<decltype(handler)>(
                   std::move(handler))] { (*shared_handler)(); };
        },
        token);
  }

  CoStatusOr<std::vector<ItemNotification>> ReadNext(
      std::size_t max_count) override {
    auto state = state_;
    while (state->notifications.empty() && !state->closed) {
      co_await ParkReader(state, boost::asio::use_awaitable);
    }
    if (state->closed) {
      co_return state->close_status ? Status{StatusCode::Bad_NoCommunication}
                                    : state->close_status;
    }

    std::vector<ItemNotification> batch;
    const std::size_t count = std::min(max_count, state->notifications.size());
    batch.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      batch.push_back(std::move(state->notifications.front()));
      state->notifications.pop_front();
    }
    co_return batch;
  }

  void Close(Status status) override {
    state_->closed = true;
    state_->close_status = std::move(status);
    state_->WakeReader();
  }

 private:
  const std::shared_ptr<State> state_;
};

}  // namespace opcua
