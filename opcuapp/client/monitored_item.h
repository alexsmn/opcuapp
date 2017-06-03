#pragma once

#include "opcuapp/client/subscription.h"

#include <functional>

namespace opcua {
namespace client {

class Subscription;

class MonitoredItem {
 public:
  explicit MonitoredItem(Subscription& subscription);
  ~MonitoredItem();

  void Subscribe(ReadValueId read_id, DataChangeHandler handler);

  void Unsubscribe();

 private:
  Subscription& subscription_;
};

inline MonitoredItem::MonitoredItem(Subscription& subscription)
    : subscription_{subscription} {
}

inline MonitoredItem::~MonitoredItem() {
  Unsubscribe();
}

inline void MonitoredItem::Subscribe(ReadValueId read_id, DataChangeHandler handler) {
  subscription_.Subscribe(*this, std::move(read_id), std::move(handler));
}

inline void MonitoredItem::Unsubscribe() {
  subscription_.Unsubscribe(*this);
}

} // namespace client
} // namespace opcua