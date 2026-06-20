#pragma once

#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/monitored_item_mock.h"
#include "opcua/scada/monitored_item_service.h"

#include <gmock/gmock.h>

namespace opcua::scada {

using MockDataChangeHandler =
    testing::MockFunction<void(const DataValue& data_value)>;

using MockEventHandler =
    testing::MockFunction<void(const Status& status, const std::any& event)>;

class MockMonitoredItemService : public MonitoredItemService {
 public:
  MockMonitoredItemService() {
    using namespace testing;

    ON_CALL(*this, CreateMonitoredItem(_, _))
        .WillByDefault(Return(default_monitored_item));
  }

  // Mockable single-item factory. `CreateSubscription` below drives this
  // through a `LegacyMonitoredItemSubscription`, so existing tests can keep
  // setting expectations on `CreateMonitoredItem`.
  MOCK_METHOD(std::shared_ptr<MonitoredItem>,
              CreateMonitoredItem,
              (const ReadValueId& value_id,
               const MonitoringParameters& params));

  StatusOr<std::unique_ptr<MonitoredItemSubscription>> CreateSubscription(
      ServiceContext /*context*/,
      MonitoredItemSubscriptionOptions options) override {
    return MakeItemFactorySubscription(
        [this](const ReadValueId& value_id,
               const MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  const std::shared_ptr<MockMonitoredItem> default_monitored_item =
      std::make_shared<testing::NiceMock<MockMonitoredItem>>();
};

}  // namespace opcua::scada
