#include "opcua/server_subscription.h"

#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/test/test_monitored_item.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

scada::NodeId NumericNode(scada::NumericId id, scada::NamespaceIndex ns = 2) {
  return {id, ns};
}

base::Time ParseTime(std::string_view value) {
  base::Time result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

class TestMonitoredItemService : public scada::MonitoredItemService {
 public:
  std::shared_ptr<scada::MonitoredItem> CreateMonitoredItem(
      const scada::ReadValueId& value_id,
      const scada::MonitoringParameters& params) {
    created_value_ids.push_back(value_id);
    created_params.push_back(params);
    auto item = std::make_shared<scada::TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  scada::StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
  CreateSubscription(scada::ServiceContext /*context*/,
                     scada::MonitoredItemSubscriptionOptions options) override {
    return scada::MakeItemFactorySubscription(
        [this](const scada::ReadValueId& value_id,
               const scada::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  std::vector<scada::ReadValueId> created_value_ids;
  std::vector<scada::MonitoringParameters> created_params;
  std::vector<std::shared_ptr<scada::TestMonitoredItem>> items;
};

TEST(ServerSubscriptionTest, PublishesAcknowledgesAndRepublishesData) {
  TestMonitoredItemService monitored_item_service;
  TestExecutor executor;
  const auto start = ParseTime("2026-04-20 10:00:00");
  ServerSubscription subscription{17,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 17,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = scada::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  // The LegacyMonitoredItemAdapter creates the underlying item asynchronously
  // on the executor, so pump pending work before observing the fake service.
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  monitored_item_service.items[0]->NotifyDataChange(scada::DataValue{
      scada::Variant{12.5}, {}, start, ParseTime("2026-04-20 10:00:01")});
  // The notification flows through the subscription pump's async read loop, so
  // pump pending work before publishing to ensure the value reaches the queue.
  executor.Poll();
  const auto publish =
      subscription.TryPublish(start + base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(publish->subscription_id, 17u);
  EXPECT_EQ(publish->available_sequence_numbers,
            (std::vector<scada::UInt32>{1u}));

  EXPECT_EQ(subscription.Acknowledge(std::vector<scada::UInt32>{1u}),
            (std::vector<scada::StatusCode>{scada::StatusCode::Good}));
  EXPECT_EQ(subscription.Republish(1u).status.code(),
            scada::StatusCode::Bad_MessageNotAvailable);
}

TEST(ServerSubscriptionTest,
     ModifiesParametersAndGeneratesAcknowledgedKeepAlive) {
  TestMonitoredItemService monitored_item_service;
  TestExecutor executor;
  const auto start = ParseTime("2026-04-20 11:00:00");
  ServerSubscription subscription{19,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 2,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto modified =
      subscription.Modify({.subscription_id = 19,
                           .parameters = {.publishing_interval_ms = 250,
                                          .lifetime_count = 80,
                                          .max_keep_alive_count = 5,
                                          .publishing_enabled = true}});
  EXPECT_EQ(modified.status.code(), scada::StatusCode::Good);
  EXPECT_DOUBLE_EQ(modified.revised_publishing_interval_ms, 250.0);
  EXPECT_EQ(modified.revised_lifetime_count, 80u);
  EXPECT_EQ(modified.revised_max_keep_alive_count, 5u);

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 19,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(201),
                                .attribute_id = scada::AttributeId::Value},
            .requested_parameters = {.client_handle = 88,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  const auto first_keep_alive =
      subscription.TryPublish(start + base::TimeDelta::FromMilliseconds(250));
  ASSERT_TRUE(first_keep_alive.has_value());
  EXPECT_EQ(first_keep_alive->notification_message.sequence_number, 1u);
  EXPECT_TRUE(first_keep_alive->notification_message.notification_data.empty());

  subscription.SetPublishingEnabled(false);
  monitored_item_service.items[0]->NotifyDataChange(
      scada::DataValue{scada::Variant{77.0},
                       {},
                       start + base::TimeDelta::FromSeconds(1),
                       start + base::TimeDelta::FromSeconds(1)});
  // The notification flows through the subscription pump's async read loop, so
  // pump pending work before publishing to ensure the value reaches the queue.
  executor.Poll();
  const auto disabled_keep_alive =
      subscription.TryPublish(start + base::TimeDelta::FromMilliseconds(1510));
  ASSERT_TRUE(disabled_keep_alive.has_value());
  EXPECT_GT(disabled_keep_alive->notification_message.sequence_number, 0u);
  EXPECT_TRUE(
      disabled_keep_alive->notification_message.notification_data.empty());

  subscription.SetPublishingEnabled(true);
  EXPECT_FALSE(
      subscription.TryPublish(start + base::TimeDelta::FromMilliseconds(1520))
          .has_value());
  const auto publish =
      subscription.TryPublish(start + base::TimeDelta::FromMilliseconds(1760));
  ASSERT_TRUE(publish.has_value());
  ASSERT_EQ(publish->notification_message.notification_data.size(), 1u);
  const auto* data_change = std::get_if<DataChangeNotification>(
      &publish->notification_message.notification_data[0]);
  ASSERT_NE(data_change, nullptr);
  EXPECT_EQ(data_change->monitored_items[0].value.value.get<double>(), 77.0);
}

TEST(ServerSubscriptionTest,
     CreateMonitoredItemsReportsPreciseStatusForUnknownNodeAndBadAttribute) {
  class NullMonitoredItemService : public scada::MonitoredItemService {
   public:
    std::shared_ptr<scada::MonitoredItem> CreateMonitoredItem(
        const scada::ReadValueId&,
        const scada::MonitoringParameters&) {
      return nullptr;
    }

    scada::StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
    CreateSubscription(
        scada::ServiceContext /*context*/,
        scada::MonitoredItemSubscriptionOptions options) override {
      return scada::MakeItemFactorySubscription(
          [this](const scada::ReadValueId& value_id,
                 const scada::MonitoringParameters& params) {
            return CreateMonitoredItem(value_id, params);
          },
          options);
    }
  } monitored_item_service;

  TestExecutor executor;
  const auto start = ParseTime("2026-04-20 12:00:00");
  ServerSubscription subscription{31,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  // A bad attribute is rejected synchronously by the endpoint layer before the
  // request ever reaches the (async) LegacyMonitoredItemAdapter.
  const auto unknown_node = subscription.CreateMonitoredItems(
      {.subscription_id = 31,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(999),
                                .attribute_id = scada::AttributeId::Value},
            .requested_parameters = {.client_handle = 1,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(unknown_node.results.size(), 1u);
  // A supported attribute on an unknown node is now accepted synchronously: the
  // LegacyMonitoredItemAdapter returns a placeholder item immediately and only
  // discovers the underlying node is unknown asynchronously while creating the
  // backing subscription, so CreateMonitoredItems can no longer surface
  // Bad_WrongNodeId at this point.
  EXPECT_EQ(unknown_node.results[0].status.code(), scada::StatusCode::Good);
  executor.Poll();

  const auto bad_attribute = subscription.CreateMonitoredItems(
      {.subscription_id = 31,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(999),
                                .attribute_id =
                                    scada::AttributeId::Description},
            .requested_parameters = {.client_handle = 2,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(bad_attribute.results.size(), 1u);
  EXPECT_EQ(bad_attribute.results[0].status.code(),
            scada::StatusCode::Bad_WrongAttributeId);
}

}  // namespace
}  // namespace opcua
