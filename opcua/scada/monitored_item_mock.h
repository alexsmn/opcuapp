#pragma once

#include "opcua/scada/monitored_item.h"

#include <gmock/gmock.h>

namespace opcua {
namespace scada {

class MockMonitoredItem : public MonitoredItem {
 public:
  MockMonitoredItem() {
    ON_CALL(
        *this,
        Subscribe(testing::VariantWith<opcua::scada::DataChangeHandler>(testing::_)))
        .WillByDefault(testing::Invoke([this](MonitoredItemHandler handler) {
          data_change_handler =
              std::move(std::get<opcua::scada::DataChangeHandler>(handler));
        }));
  }

  MOCK_METHOD(void, Subscribe, (MonitoredItemHandler handler), (override));

  opcua::scada::DataChangeHandler data_change_handler;
};

}  // namespace scada
}  // namespace opcua (vendored)
