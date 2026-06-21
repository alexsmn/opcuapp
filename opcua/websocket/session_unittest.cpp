#include "opcua/server_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/test/test_monitored_item.h"

#include <gtest/gtest.h>
#include <thread>
#include <type_traits>

namespace opcua::ws {
namespace {

// The monitored-item subscription pump parks its read loop on an asio
// steady_timer; resuming it after a notification is pushed (or after an item is
// first wired up) needs the timer service to settle, which a single `Drain`
// does not guarantee. Spin the executor so async notification delivery
// completes before assertions.
inline void DrainPump(opcua::TestExecutor& executor) {
  for (int i = 0; i < 200; ++i) {
    Drain(executor);
    std::this_thread::yield();
  }
}

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

ServerSession MakeSession(opcua::AnyExecutor executor,
                          opcua::scada::MonitoredItemService& monitored_item_service,
                          const std::function<opcua::base::Time()>& now) {
  return ServerSession{{
      .session_id = NumericNode(1001),
      .authentication_token = NumericNode(2001, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(77, 4)),
      .executor = std::move(executor),
      .monitored_item_service = monitored_item_service,
      .now = now,
  }};
}

TEST(SessionTest, PublishesAcrossSubscriptionsRoundRobinAndAcknowledges) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 14:00:00");
  auto session =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  const auto first_subscription =
      session.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                 .lifetime_count = 60,
                                                 .max_keep_alive_count = 3,
                                                 .publishing_enabled = true}});
  const auto second_subscription =
      session.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                 .lifetime_count = 60,
                                                 .max_keep_alive_count = 3,
                                                 .publishing_enabled = true}});

  const auto first_items = session.CreateMonitoredItems(
      {.subscription_id = first_subscription.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(1),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 11,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  const auto second_items = session.CreateMonitoredItems(
      {.subscription_id = second_subscription.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(2),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 22,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(first_items.results.size(), 1u);
  ASSERT_EQ(second_items.results.size(), 1u);
  // Monitored items are created asynchronously on the executor.
  DrainPump(executor);
  ASSERT_EQ(monitored_item_service.items.size(), 2u);

  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{10.0}, {}, now, now});
  monitored_item_service.items[1]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{20.0}, {}, now, now});
  DrainPump(executor);

  now = now + opcua::base::TimeDelta::FromMilliseconds(100);
  const auto first_publish = session.Publish({});
  EXPECT_EQ(first_publish.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(first_publish.subscription_id, first_subscription.subscription_id);
  const auto* first_data = std::get_if<DataChangeNotification>(
      &first_publish.notification_message.notification_data[0]);
  ASSERT_NE(first_data, nullptr);
  EXPECT_EQ(first_data->monitored_items[0].client_handle, 11u);

  now = now + opcua::base::TimeDelta::FromMilliseconds(100);
  const auto second_publish = session.Publish(
      {.subscription_acknowledgements = {
           {.subscription_id = first_subscription.subscription_id,
            .sequence_number = 1}}});
  EXPECT_EQ(second_publish.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(second_publish.subscription_id,
            second_subscription.subscription_id);
  EXPECT_EQ(second_publish.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  const auto* second_data = std::get_if<DataChangeNotification>(
      &second_publish.notification_message.notification_data[0]);
  ASSERT_NE(second_data, nullptr);
  EXPECT_EQ(second_data->monitored_items[0].client_handle, 22u);

  const auto republish =
      session.Republish({.subscription_id = second_subscription.subscription_id,
                         .retransmit_sequence_number = 1});
  EXPECT_EQ(republish.status.code(), opcua::StatusCode::Good);
}

TEST(SessionTest, PrimesKeepAliveAndHonorsPublishingMode) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 15:00:00");
  auto session =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  const auto created =
      session.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                 .lifetime_count = 60,
                                                 .max_keep_alive_count = 2,
                                                 .publishing_enabled = true}});

  const auto first_publish = session.Publish({});
  EXPECT_EQ(first_publish.status.code(), opcua::StatusCode::Good);
  EXPECT_TRUE(first_publish.notification_message.notification_data.empty());
  EXPECT_EQ(first_publish.subscription_id, 0u);

  now = now + opcua::base::TimeDelta::FromMilliseconds(100);
  const auto keep_alive = session.Publish({});
  EXPECT_EQ(keep_alive.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(keep_alive.subscription_id, created.subscription_id);
  EXPECT_EQ(keep_alive.notification_message.sequence_number, 1u);

  const auto mode = session.SetPublishingMode(
      {.publishing_enabled = false,
       .subscription_ids = {created.subscription_id}});
  EXPECT_EQ(mode.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  now = now + opcua::base::TimeDelta::FromMilliseconds(300);
  const auto disabled_publish = session.Publish({});
  EXPECT_EQ(disabled_publish.status.code(), opcua::StatusCode::Good);
  EXPECT_TRUE(disabled_publish.notification_message.notification_data.empty());
  EXPECT_EQ(disabled_publish.subscription_id, created.subscription_id);
  EXPECT_EQ(disabled_publish.notification_message.sequence_number, 1u);
}

TEST(SessionTest,
     PollPublishRechecksWithinPublishingIntervalBeforeKeepAliveDeadline) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 15:30:00");
  auto session =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  session.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                             .lifetime_count = 60,
                                             .max_keep_alive_count = 10,
                                             .publishing_enabled = true}});

  const auto poll = session.PollPublish();
  ASSERT_FALSE(poll.response.has_value());
  ASSERT_TRUE(poll.wait_for.has_value());
  EXPECT_EQ(*poll.wait_for, opcua::base::TimeDelta::FromMilliseconds(100));
}

TEST(SessionTest, RoutesMonitoredItemOperationsToSubscription) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 16:00:00");
  auto session =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  const auto subscription =
      session.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                 .lifetime_count = 60,
                                                 .max_keep_alive_count = 3,
                                                 .publishing_enabled = true}});

  const auto created = session.CreateMonitoredItems(
      {.subscription_id = subscription.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(301),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 7,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(created.results.size(), 1u);
  const auto monitored_item_id = created.results[0].monitored_item_id;

  const auto modified = session.ModifyMonitoredItems(
      {.subscription_id = subscription.subscription_id,
       .items_to_modify = {
           {.monitored_item_id = monitored_item_id,
            .requested_parameters = {.client_handle = 8,
                                     .sampling_interval_ms = 50,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(modified.results.size(), 1u);
  EXPECT_EQ(modified.results[0].status.code(), opcua::StatusCode::Good);

  const auto monitoring_mode = session.SetMonitoringMode(
      {.subscription_id = subscription.subscription_id,
       .monitoring_mode = MonitoringMode::Sampling,
       .monitored_item_ids = {monitored_item_id}});
  EXPECT_EQ(monitoring_mode.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  const auto deleted_items = session.DeleteMonitoredItems(
      {.subscription_id = subscription.subscription_id,
       .monitored_item_ids = {monitored_item_id}});
  EXPECT_EQ(deleted_items.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  const auto deleted_subscriptions = session.DeleteSubscriptions(
      {.subscription_ids = {subscription.subscription_id}});
  EXPECT_EQ(deleted_subscriptions.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
}

TEST(SessionTest, TransfersSubscriptionsBetweenSessions) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 17:00:00");
  auto source =
      MakeSession(executor, monitored_item_service, [&] { return now; });
  auto target =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  const auto created =
      source.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                .lifetime_count = 60,
                                                .max_keep_alive_count = 3,
                                                .publishing_enabled = true}});
  const auto items = source.CreateMonitoredItems(
      {.subscription_id = created.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(401),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 41,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(items.results.size(), 1u);
  // Monitored items are created asynchronously on the executor.
  DrainPump(executor);
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{55.0}, {}, now, now});
  DrainPump(executor);

  const auto transferred = target.TransferSubscriptionsFrom(
      source, {.subscription_ids = {created.subscription_id},
               .send_initial_values = true});
  EXPECT_EQ(transferred.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  // The source session no longer owns any subscriptions (OPC UA Part 4 §5.13.5).
  const auto source_publish = source.Publish({});
  EXPECT_EQ(source_publish.status.code(),
            opcua::StatusCode::Bad_NoSubscription);

  now = now + opcua::base::TimeDelta::FromMilliseconds(100);
  const auto target_publish = target.Publish({});
  EXPECT_EQ(target_publish.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(target_publish.subscription_id, created.subscription_id);
  const auto* data = std::get_if<DataChangeNotification>(
      &target_publish.notification_message.notification_data[0]);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->monitored_items[0].value.value.get<double>(), 55.0);
}

TEST(SessionTest, StoresBrowseContinuationPointsAndResumesPages) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 17:30:00");
  auto session =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  auto paged = session.StoreBrowseResults(
      {.status = opcua::StatusCode::Good,
       .results = {{.status_code = opcua::StatusCode::Good,
                    .references = {{.reference_type_id = NumericNode(501),
                                    .forward = true,
                                    .node_id = NumericNode(601)},
                                   {.reference_type_id = NumericNode(502),
                                    .forward = true,
                                    .node_id = NumericNode(602)},
                                   {.reference_type_id = NumericNode(503),
                                    .forward = false,
                                    .node_id = NumericNode(603)}}}}},
      2);

  ASSERT_EQ(paged.results.size(), 1u);
  ASSERT_EQ(paged.results[0].references.size(), 2u);
  ASSERT_FALSE(paged.results[0].continuation_point.empty());
  EXPECT_EQ(paged.results[0].references[0].node_id, NumericNode(601));
  EXPECT_EQ(paged.results[0].references[1].node_id, NumericNode(602));

  const auto next = session.BrowseNext(
      {.continuation_points = {paged.results[0].continuation_point}});
  EXPECT_EQ(next.status.code(), opcua::StatusCode::Good);
  ASSERT_EQ(next.results.size(), 1u);
  EXPECT_EQ(next.results[0].status_code, opcua::StatusCode::Good);
  ASSERT_EQ(next.results[0].references.size(), 1u);
  EXPECT_TRUE(next.results[0].continuation_point.empty());
  EXPECT_EQ(next.results[0].references[0].node_id, NumericNode(603));

  const auto invalid = session.BrowseNext(
      {.continuation_points = {paged.results[0].continuation_point}});
  ASSERT_EQ(invalid.results.size(), 1u);
  EXPECT_EQ(invalid.results[0].status_code, opcua::StatusCode::Bad_WrongIndex);
}

TEST(SessionTest, ReleasesBrowseContinuationPointsWithoutReturningData) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 17:40:00");
  auto session =
      MakeSession(executor, monitored_item_service, [&] { return now; });

  auto paged = session.StoreBrowseResults(
      {.status = opcua::StatusCode::Good,
       .results = {{.status_code = opcua::StatusCode::Good,
                    .references = {{.reference_type_id = NumericNode(701),
                                    .forward = true,
                                    .node_id = NumericNode(801)},
                                   {.reference_type_id = NumericNode(702),
                                    .forward = true,
                                    .node_id = NumericNode(802)}}}}},
      1);

  const auto released = session.BrowseNext(
      {.release_continuation_points = true,
       .continuation_points = {paged.results[0].continuation_point}});
  ASSERT_EQ(released.results.size(), 1u);
  EXPECT_EQ(released.results[0].status_code, opcua::StatusCode::Good);
  EXPECT_TRUE(released.results[0].references.empty());

  const auto invalid = session.BrowseNext(
      {.continuation_points = {paged.results[0].continuation_point}});
  ASSERT_EQ(invalid.results.size(), 1u);
  EXPECT_EQ(invalid.results[0].status_code, opcua::StatusCode::Bad_WrongIndex);
}

}  // namespace
}  // namespace opcua::ws
