#pragma once

#include "opcua/scada/data_value.h"
#include "opcua/scada/monitored_item.h"
#include "opcua/scada/status.h"

#include <condition_variable>
#include <mutex>

namespace opcua {
namespace scada {

// TODO: Must be thread-safe, as it's used in history tests. Historical
// databases run in separate threads.
class TestMonitoredItem : public MonitoredItem {
 public:
  virtual void Subscribe(MonitoredItemHandler handler) override {
    assert(!data_change_handler_);
    assert(!event_handler_);

    if (auto* data_change_handler = std::get_if<DataChangeHandler>(&handler)) {
      data_change_handler_ = std::move(*data_change_handler);
      if (!data_value_.is_null()) {
        data_change_handler_(data_value_);
      }
    } else if (auto* event_handler = std::get_if<EventHandler>(&handler)) {
      event_handler_ = std::move(*event_handler);
    } else {
      assert(false);
    }

    {
      std::lock_guard lock{mutex_};
      subscribed_ = true;
    }
    subscribed_cv_.notify_all();
  }

  bool subscribed() const { return event_handler_ != nullptr; }

  void WaitForSubscription() {
    std::unique_lock lock{mutex_};
    subscribed_cv_.wait(lock, [this] { return subscribed_; });
  }

  void NotifyDataChange(const opcua::scada::DataValue& data_value) {
    data_value_ = data_value;

    if (data_change_handler_) {
      data_change_handler_(data_value);
    }
  }

  void NotifyEvent(const std::any& event) {
    if (!event_handler_)
      return;

    event_handler_(StatusCode::Good, event);
  }

 private:
  DataChangeHandler data_change_handler_;
  EventHandler event_handler_;

  std::mutex mutex_;
  std::condition_variable subscribed_cv_;
  bool subscribed_ = false;

  // TODO: Thread-safe.
  opcua::scada::DataValue data_value_;
};

}  // namespace scada
}  // namespace opcua (vendored)
