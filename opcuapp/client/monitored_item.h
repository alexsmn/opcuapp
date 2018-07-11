#pragma once

#include "types.h"

#include <functional>

namespace opcua {
namespace client {

class Subscription;

class MonitoredItem {
 public:
  explicit MonitoredItem(Subscription& subscription);
  ~MonitoredItem();

  using DataChangeHandler = std::function<void(DataValue value)>;
  void Create(DataChangeHandler handler);

  void Delete();

 private:
  Subscription& subscription_;
};

inline MonitoredItem::MonitoredItem(Subscription& subscription) subscription_{
    subscription} {}

inline MonitoredItem::~MonitoredItem() {
  subscription_.RemoveMonitoredItem(*this);
}

inline void MonitoredItem::Create(DataChangeHandler handler) {
  handler_ = std::move(handler);
  subscription_.AddMonitoredItem(*this, std::move(handler));
}

inline void MonitoredItem::Delete() {
  subscription_.RemoveMonitoredItem(*this);
}

}  // namespace client
}  // namespace opcua
