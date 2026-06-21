#include "opcua/server_session.h"

#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/scada/item_factory_subscription.h"
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
      const opcua::MonitoringParameters& params) {
    (void)value_id;
    (void)params;
    auto item = std::make_shared<opcua::TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  opcua::StatusOr<std::unique_ptr<opcua::scada::MonitoredItemSubscription>>
  CreateSubscription(opcua::ServiceContext /*context*/,
                     opcua::scada::MonitoredItemSubscriptionOptions options) override {
    return opcua::scada::MakeItemFactorySubscription(
        [this](const opcua::ReadValueId& value_id,
               const opcua::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  std::vector<std::shared_ptr<opcua::TestMonitoredItem>> items;
};

TEST(ServerSessionTest, StoresContinuationPointsAndTransfersSubscriptions) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 17:00:00");
  ServerSession source{{
      .session_id = NumericNode(1001),
      .authentication_token = NumericNode(2001, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(77, 4)),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};
  ServerSession target{{
      .session_id = NumericNode(1002),
      .authentication_token = NumericNode(2002, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(78, 4)),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};

  const auto created =
      source.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                .lifetime_count = 60,
                                                .max_keep_alive_count = 3,
                                                .publishing_enabled = true}});
  EXPECT_TRUE(source.HasSubscription(created.subscription_id));

  auto paged = source.StoreBrowseResults(
      {.status = opcua::StatusCode::Good,
       .results = {{.status_code = opcua::StatusCode::Good,
                    .references = {{.reference_type_id = NumericNode(501),
                                    .forward = true,
                                    .node_id = NumericNode(601)},
                                   {.reference_type_id = NumericNode(502),
                                    .forward = true,
                                    .node_id = NumericNode(602)}}}}},
      1);
  ASSERT_EQ(paged.results.size(), 1u);
  ASSERT_FALSE(paged.results[0].continuation_point.empty());

  const auto next = source.BrowseNext(
      {.continuation_points = {paged.results[0].continuation_point}});
  ASSERT_EQ(next.results.size(), 1u);
  EXPECT_EQ(next.results[0].status_code, opcua::StatusCode::Good);
  ASSERT_EQ(next.results[0].references.size(), 1u);
  EXPECT_EQ(next.results[0].references[0].node_id, NumericNode(602));

  const auto transferred = target.TransferSubscriptionsFrom(
      source, {.subscription_ids = {created.subscription_id},
               .send_initial_values = true});
  EXPECT_EQ(transferred.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  EXPECT_FALSE(source.HasSubscription(created.subscription_id));
  EXPECT_TRUE(target.HasSubscription(created.subscription_id));
}

TEST(ServerSessionTest, RejectsBrowseWhenContinuationPointLimitReached) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 17:00:00");
  ServerSession session{{
      .session_id = NumericNode(1001),
      .authentication_token = NumericNode(2001, 3),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};

  // Each Browse result has two references with a page size of one, so each
  // pages and allocates one continuation point.
  const auto page_once = [&] {
    return session.StoreBrowseResults(
        {.status = opcua::StatusCode::Good,
         .results = {{.status_code = opcua::StatusCode::Good,
                      .references = {{.reference_type_id = NumericNode(501),
                                      .forward = true,
                                      .node_id = NumericNode(601)},
                                     {.reference_type_id = NumericNode(502),
                                      .forward = true,
                                      .node_id = NumericNode(602)}}}}},
        1);
  };

  opcua::ByteString first_continuation_point;
  for (std::uint32_t i = 0; i < kMaxBrowseContinuationPoints; ++i) {
    const auto paged = page_once();
    ASSERT_EQ(paged.results[0].status_code, opcua::StatusCode::Good);
    ASSERT_FALSE(paged.results[0].continuation_point.empty());
    if (i == 0)
      first_continuation_point = paged.results[0].continuation_point;
  }

  // The next allocation is refused.
  const auto overflow = page_once();
  ASSERT_EQ(overflow.results.size(), 1u);
  EXPECT_EQ(overflow.results[0].status_code,
            opcua::StatusCode::Bad_NoContinuationPoints);
  EXPECT_TRUE(overflow.results[0].continuation_point.empty());

  // Releasing a continuation point frees a slot so Browse can page again.
  const auto released = session.BrowseNext(
      {.continuation_points = {first_continuation_point},
       .release_continuation_points = true});
  ASSERT_EQ(released.results.size(), 1u);
  EXPECT_EQ(released.results[0].status_code, opcua::StatusCode::Good);

  const auto after_release = page_once();
  EXPECT_EQ(after_release.results[0].status_code, opcua::StatusCode::Good);
  EXPECT_FALSE(after_release.results[0].continuation_point.empty());
}

TEST(ServerSessionTest,
     TransfersSubscriptionsAcrossSessionsAndPublishesQueuedData) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 18:00:00");
  ServerSession source{{
      .session_id = NumericNode(1101),
      .authentication_token = NumericNode(2101, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(77, 4)),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};
  ServerSession target{{
      .session_id = NumericNode(1102),
      .authentication_token = NumericNode(2102, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(78, 4)),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};

  const auto created_subscription =
      source.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                .lifetime_count = 60,
                                                .max_keep_alive_count = 3,
                                                .publishing_enabled = true}});
  const auto created_items = source.CreateMonitoredItems(
      {.subscription_id = created_subscription.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(301),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(created_items.results.size(), 1u);
  ASSERT_EQ(created_items.results[0].status.code(), opcua::StatusCode::Good);
  // The backing monitored item is created asynchronously on the executor.
  executor.Poll();
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  const auto transferred = target.TransferSubscriptionsFrom(
      source, {.subscription_ids = {created_subscription.subscription_id},
               .send_initial_values = true});
  EXPECT_EQ(transferred.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  EXPECT_FALSE(source.HasSubscription(created_subscription.subscription_id));
  EXPECT_TRUE(target.HasSubscription(created_subscription.subscription_id));

  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Double{77.0}, {}, now, now});
  // The notification flows through the subscription pump's async read loop, so
  // pump pending work before publishing to ensure the value reaches the queue.
  executor.Poll();
  now = now + opcua::base::TimeDelta::FromMilliseconds(100);

  const auto published = target.Publish({});
  EXPECT_EQ(published.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(published.subscription_id, created_subscription.subscription_id);
  ASSERT_EQ(published.notification_message.notification_data.size(), 1u);
  const auto* data_change = std::get_if<DataChangeNotification>(
      &published.notification_message.notification_data[0]);
  ASSERT_NE(data_change, nullptr);
  ASSERT_EQ(data_change->monitored_items.size(), 1u);
  EXPECT_EQ(data_change->monitored_items[0].client_handle, 44u);
  EXPECT_EQ(data_change->monitored_items[0].value.value.get<double>(), 77.0);
}

TEST(ServerSessionTest, ModifiesSubscriptionsAndRoutesMonitoredItemOperations) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 19:00:00");
  ServerSession session{{
      .session_id = NumericNode(1201),
      .authentication_token = NumericNode(2201, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(79, 4)),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};

  const auto created_subscription =
      session.CreateSubscription({.parameters = {.publishing_interval_ms = 100,
                                                 .lifetime_count = 60,
                                                 .max_keep_alive_count = 3,
                                                 .publishing_enabled = true}});
  const auto modified_subscription = session.ModifySubscription(
      {.subscription_id = created_subscription.subscription_id,
       .parameters = {.publishing_interval_ms = 250,
                      .lifetime_count = 80,
                      .max_keep_alive_count = 5,
                      .publishing_enabled = true}});
  EXPECT_EQ(modified_subscription.status.code(), opcua::StatusCode::Good);
  EXPECT_DOUBLE_EQ(modified_subscription.revised_publishing_interval_ms, 250.0);
  EXPECT_EQ(modified_subscription.revised_lifetime_count, 80u);
  EXPECT_EQ(modified_subscription.revised_max_keep_alive_count, 5u);

  const auto created_items = session.CreateMonitoredItems(
      {.subscription_id = created_subscription.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(401),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 7,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(created_items.results.size(), 1u);
  ASSERT_EQ(created_items.results[0].status.code(), opcua::StatusCode::Good);
  const auto monitored_item_id = created_items.results[0].monitored_item_id;

  const auto modified_items = session.ModifyMonitoredItems(
      {.subscription_id = created_subscription.subscription_id,
       .items_to_modify = {
           {.monitored_item_id = monitored_item_id,
            .requested_parameters = {.client_handle = 8,
                                     .sampling_interval_ms = 50,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(modified_items.results.size(), 1u);
  EXPECT_EQ(modified_items.results[0].status.code(), opcua::StatusCode::Good);

  const auto monitoring_mode = session.SetMonitoringMode(
      {.subscription_id = created_subscription.subscription_id,
       .monitoring_mode = MonitoringMode::Sampling,
       .monitored_item_ids = {monitored_item_id}});
  EXPECT_EQ(monitoring_mode.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  const auto disable_publishing = session.SetPublishingMode(
      {.publishing_enabled = false,
       .subscription_ids = {created_subscription.subscription_id}});
  EXPECT_EQ(disable_publishing.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  const auto deleted_items = session.DeleteMonitoredItems(
      {.subscription_id = created_subscription.subscription_id,
       .monitored_item_ids = {monitored_item_id}});
  EXPECT_EQ(deleted_items.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  const auto deleted_subscriptions = session.DeleteSubscriptions(
      {.subscription_ids = {created_subscription.subscription_id}});
  EXPECT_EQ(deleted_subscriptions.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  EXPECT_FALSE(session.HasSubscription(created_subscription.subscription_id));
}

TEST(ServerSessionTest, PublishWithoutSubscriptionsReturnsBadNoSubscription) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  auto now = ParseTime("2026-04-20 17:00:00");
  ServerSession session{{
      .session_id = NumericNode(1001),
      .authentication_token = NumericNode(2001, 3),
      .service_context =
          opcua::ServiceContext{}.with_user_id(NumericNode(77, 4)),
      .executor = executor,
      .monitored_item_service = monitored_item_service,
      .now = [&] { return now; },
  }};

  // OPC UA Part 4 §5.13.5: a Publish for a session with no subscriptions is
  // answered with Bad_NoSubscription.
  const auto published = session.Publish({});
  EXPECT_EQ(published.status.code(),
            opcua::StatusCode::Bad_NoSubscription);
}

}  // namespace
}  // namespace opcua
