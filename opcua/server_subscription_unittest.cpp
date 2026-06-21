#include "opcua/server_subscription.h"

#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/test/test_monitored_item.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

opcua::NodeId NumericNode(opcua::NumericId id, opcua::NamespaceIndex ns = 2) {
  return {id, ns};
}

opcua::base::Time ParseTime(std::string_view value) {
  opcua::base::Time result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

class TestMonitoredItemService : public opcua::scada::MonitoredItemService {
 public:
  std::shared_ptr<opcua::scada::MonitoredItem> CreateMonitoredItem(
      const opcua::ReadValueId& value_id,
      const opcua::scada::MonitoringParameters& params) {
    created_value_ids.push_back(value_id);
    created_params.push_back(params);
    auto item = std::make_shared<opcua::TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  opcua::StatusOr<std::unique_ptr<opcua::scada::MonitoredItemSubscription>>
  CreateSubscription(opcua::ServiceContext /*context*/,
                     opcua::scada::MonitoredItemSubscriptionOptions options) override {
    return opcua::scada::MakeItemFactorySubscription(
        [this](const opcua::ReadValueId& value_id,
               const opcua::scada::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  std::vector<opcua::ReadValueId> created_value_ids;
  std::vector<opcua::scada::MonitoringParameters> created_params;
  std::vector<std::shared_ptr<opcua::TestMonitoredItem>> items;
};

TEST(ServerSubscriptionTest, PublishesAcknowledgesAndRepublishesData) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
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
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  // The LegacyMonitoredItemAdapter creates the underlying item asynchronously
  // on the executor, so pump pending work before observing the fake service.
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  monitored_item_service.items[0]->NotifyDataChange(opcua::DataValue{
      opcua::Variant{12.5}, {}, start, ParseTime("2026-04-20 10:00:01")});
  // The notification flows through the subscription pump's async read loop, so
  // pump pending work before publishing to ensure the value reaches the queue.
  executor.Poll();
  const auto publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(publish->subscription_id, 17u);
  EXPECT_EQ(publish->available_sequence_numbers,
            (std::vector<opcua::UInt32>{1u}));

  EXPECT_EQ(subscription.Acknowledge(std::vector<opcua::UInt32>{1u}),
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  EXPECT_EQ(subscription.Republish(1u).status.code(),
            opcua::StatusCode::Bad_MessageNotAvailable);
}

TEST(ServerSubscriptionTest,
     ModifiesParametersAndGeneratesAcknowledgedKeepAlive) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
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
  EXPECT_EQ(modified.status.code(), opcua::StatusCode::Good);
  EXPECT_DOUBLE_EQ(modified.revised_publishing_interval_ms, 250.0);
  EXPECT_EQ(modified.revised_lifetime_count, 80u);
  EXPECT_EQ(modified.revised_max_keep_alive_count, 5u);

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 19,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(201),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 88,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  const auto first_keep_alive =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(250));
  ASSERT_TRUE(first_keep_alive.has_value());
  EXPECT_EQ(first_keep_alive->notification_message.sequence_number, 1u);
  EXPECT_TRUE(first_keep_alive->notification_message.notification_data.empty());

  subscription.SetPublishingEnabled(false);
  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{77.0},
                       {},
                       start + opcua::base::TimeDelta::FromSeconds(1),
                       start + opcua::base::TimeDelta::FromSeconds(1)});
  // The notification flows through the subscription pump's async read loop, so
  // pump pending work before publishing to ensure the value reaches the queue.
  executor.Poll();
  const auto disabled_keep_alive =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(1510));
  ASSERT_TRUE(disabled_keep_alive.has_value());
  EXPECT_GT(disabled_keep_alive->notification_message.sequence_number, 0u);
  EXPECT_TRUE(
      disabled_keep_alive->notification_message.notification_data.empty());

  subscription.SetPublishingEnabled(true);
  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(1520))
          .has_value());
  const auto publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(1760));
  ASSERT_TRUE(publish.has_value());
  ASSERT_EQ(publish->notification_message.notification_data.size(), 1u);
  const auto* data_change = std::get_if<DataChangeNotification>(
      &publish->notification_message.notification_data[0]);
  ASSERT_NE(data_change, nullptr);
  EXPECT_EQ(data_change->monitored_items[0].value.value.get<double>(), 77.0);
}

TEST(ServerSubscriptionTest,
     CreateMonitoredItemsReportsPreciseStatusForUnknownNodeAndBadAttribute) {
  class NullMonitoredItemService : public opcua::scada::MonitoredItemService {
   public:
    std::shared_ptr<opcua::scada::MonitoredItem> CreateMonitoredItem(
        const opcua::ReadValueId&,
        const opcua::scada::MonitoringParameters&) {
      return nullptr;
    }

    opcua::StatusOr<std::unique_ptr<opcua::scada::MonitoredItemSubscription>>
    CreateSubscription(
        opcua::ServiceContext /*context*/,
        opcua::scada::MonitoredItemSubscriptionOptions options) override {
      return opcua::scada::MakeItemFactorySubscription(
          [this](const opcua::ReadValueId& value_id,
                 const opcua::scada::MonitoringParameters& params) {
            return CreateMonitoredItem(value_id, params);
          },
          options);
    }
  } monitored_item_service;

  opcua::TestExecutor executor;
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
                                .attribute_id = opcua::AttributeId::Value},
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
  EXPECT_EQ(unknown_node.results[0].status.code(), opcua::StatusCode::Good);
  executor.Poll();

  const auto bad_attribute = subscription.CreateMonitoredItems(
      {.subscription_id = 31,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(999),
                                .attribute_id =
                                    opcua::AttributeId::Description},
            .requested_parameters = {.client_handle = 2,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(bad_attribute.results.size(), 1u);
  EXPECT_EQ(bad_attribute.results[0].status.code(),
            opcua::StatusCode::Bad_WrongAttributeId);
}

TEST(ServerSubscriptionTest, RevisesLifetimeCountToAtLeastThreeKeepAlive) {
  // Requested lifetime below 3x keep-alive is raised.
  const auto revised = ServerSubscription::ReviseParameters(
      {.lifetime_count = 5, .max_keep_alive_count = 4});
  EXPECT_EQ(revised.max_keep_alive_count, 4u);
  EXPECT_EQ(revised.lifetime_count, 12u);

  // A zero keep-alive count is defaulted, and lifetime follows.
  const auto defaulted = ServerSubscription::ReviseParameters({});
  EXPECT_EQ(defaulted.max_keep_alive_count, 3u);
  EXPECT_EQ(defaulted.lifetime_count, 9u);

  // A conforming request is left unchanged.
  const auto unchanged = ServerSubscription::ReviseParameters(
      {.lifetime_count = 60, .max_keep_alive_count = 3});
  EXPECT_EQ(unchanged.lifetime_count, 60u);
}

TEST(ServerSubscriptionTest, AcknowledgeUnknownSequenceNumberReports) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  ServerSubscription subscription{
      7,
      {.publishing_interval_ms = 100,
       .lifetime_count = 60,
       .max_keep_alive_count = 3,
       .publishing_enabled = true},
      executor,
      monitored_item_service,
      ParseTime("2026-04-20 10:00:00")};

  EXPECT_EQ(subscription.Acknowledge(std::vector<opcua::UInt32>{999u}),
            (std::vector<opcua::StatusCode>{
                opcua::StatusCode::Bad_SequenceNumberUnknown}));
}

TEST(ServerSubscriptionTest, BoundsRetransmissionQueue) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 10:00:00");
  ServerSubscription subscription{17,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  constexpr std::size_t kOverflow = 5;
  const auto total =
      ServerSubscription::kMaxRetransmitQueueNotifications + kOverflow;
  subscription.CreateMonitoredItems(
      {.subscription_id = 17,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {
                .client_handle = 44,
                .queue_size = static_cast<opcua::UInt32>(total + 10),
                .discard_oldest = true}}}});
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  for (std::size_t i = 0; i < total; ++i) {
    monitored_item_service.items[0]->NotifyDataChange(opcua::DataValue{
        opcua::Variant{static_cast<double>(i)}, {}, start, start});
  }
  executor.Poll();

  std::optional<PublishResponse> last_publish;
  for (std::size_t i = 0; i < total; ++i) {
    last_publish = subscription.TryPublish(
        start + opcua::base::TimeDelta::FromMilliseconds(
                    static_cast<int64_t>((i + 1) * 100)));
    ASSERT_TRUE(last_publish.has_value());
  }

  // The retransmission queue is capped: the oldest messages were dropped.
  ASSERT_TRUE(last_publish.has_value());
  EXPECT_EQ(last_publish->available_sequence_numbers.size(),
            ServerSubscription::kMaxRetransmitQueueNotifications);
  EXPECT_EQ(subscription.Republish(1u).status.code(),
            opcua::StatusCode::Bad_MessageNotAvailable);
  EXPECT_EQ(subscription
                .Republish(static_cast<opcua::UInt32>(total))
                .status.code(),
            opcua::StatusCode::Good);
}

TEST(ServerSubscriptionTest, NonReportingMonitoringModeSuppressesDataChanges) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 10:00:00");
  ServerSubscription subscription{17,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  subscription.CreateMonitoredItems(
      {.subscription_id = 17,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = opcua::AttributeId::Value},
            .monitoring_mode = MonitoringMode::Sampling,
            .requested_parameters = {.client_handle = 44, .queue_size = 4}}}});
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  monitored_item_service.items[0]->NotifyDataChange(opcua::DataValue{
      opcua::Variant{12.5}, {}, start, start});
  executor.Poll();

  // Sampling (not Reporting) mode collects values but does not queue them for
  // publishing.
  EXPECT_FALSE(subscription.HasPendingNotifications());
}

TEST(ServerSubscriptionTest, QueueOverflowCapsQueuedDataChanges) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 10:00:00");
  ServerSubscription subscription{17,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  subscription.CreateMonitoredItems(
      {.subscription_id = 17,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  // Three values into a queue of size two: only two survive.
  for (int i = 0; i < 3; ++i) {
    monitored_item_service.items[0]->NotifyDataChange(opcua::DataValue{
        opcua::Variant{static_cast<double>(i)}, {}, start, start});
  }
  executor.Poll();

  std::size_t data_messages = 0;
  for (int i = 0; i < 4; ++i) {
    const auto publish = subscription.TryPublish(
        start + opcua::base::TimeDelta::FromMilliseconds((i + 1) * 100));
    if (publish.has_value() &&
        !publish->notification_message.notification_data.empty()) {
      ++data_messages;
    }
  }
  EXPECT_EQ(data_messages, 2u);
}

TEST(ServerSubscriptionTest, AppliesAbsoluteDeadbandFilter) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 10:00:00");
  ServerSubscription subscription{17,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .publishing_enabled = true},
                                  executor,
                                  monitored_item_service,
                                  start};

  subscription.CreateMonitoredItems(
      {.subscription_id = 17,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {
                .client_handle = 44,
                .queue_size = 10,
                .filter = MonitoringFilter{DataChangeFilter{
                    .deadband_type = DeadbandType::Absolute,
                    .deadband_value = 5.0}}}}}});
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  // 10.0 reported (first); 12.0 within the 5.0 deadband (skipped); 16.0 differs
  // from 10.0 by 6.0 (reported).
  for (const double value : {10.0, 12.0, 16.0}) {
    monitored_item_service.items[0]->NotifyDataChange(
        opcua::DataValue{opcua::Variant{value}, {}, start, start});
  }
  executor.Poll();

  std::size_t data_messages = 0;
  for (int i = 0; i < 5; ++i) {
    const auto publish = subscription.TryPublish(
        start + opcua::base::TimeDelta::FromMilliseconds((i + 1) * 100));
    if (publish.has_value() &&
        !publish->notification_message.notification_data.empty()) {
      ++data_messages;
    }
  }
  EXPECT_EQ(data_messages, 2u);
}

}  // namespace
}  // namespace opcua
