#include "opcua/session/server_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/monitored/test/fake_monitored_item_subscription.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

namespace opcua {
namespace {

using BackingStates =
    std::vector<std::shared_ptr<FakeMonitoredItemSubscription::State>>;

NodeId NumericNode(NumericId id, NamespaceIndex ns = 2) {
  return {id, ns};
}

DateTime ParseTime(std::string_view value) {
  DateTime result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

// A ServerSession and the pieces a test drives it with: the executor its
// subscriptions run their binding and read loops on, a clock it can advance,
// and the backing subscriptions handed out on CreateSubscription — one State
// per subscription, in creation order.
class SessionHarness {
 public:
  SessionHarness(NumericId session_id,
                 DateTime now,
                 TestExecutor executor,
                 std::shared_ptr<BackingStates> backing_states)
      : now_{now},
        executor_{executor},
        backing_states_{std::move(backing_states)},
        session_{{
            .session_id = NumericNode(session_id),
            .authentication_token = NumericNode(session_id + 1000, 3),
            .service_context =
                ServiceContext{}.with_user_id(NumericNode(77, 4)),
            .executor = executor,
            .create_subscription =
                FakeMonitoredItemSubscription::MakeCreateSubscriptionPerCall(
                    executor,
                    backing_states_),
            .now = [this] { return now_; },
        }} {}

  // A session with its own executor, clock and backing states.
  explicit SessionHarness(NumericId session_id, DateTime now)
      : SessionHarness(session_id,
                       now,
                       TestExecutor{},
                       std::make_shared<BackingStates>()) {}

  ServerSession& session() { return session_; }
  TestExecutor executor() const { return executor_; }
  const std::shared_ptr<BackingStates>& backing_states() const {
    return backing_states_;
  }
  FakeMonitoredItemSubscription::State& backing(std::size_t index) const {
    return *backing_states_->at(index);
  }

  DateTime now() const { return now_; }
  void Advance(int64_t ms) { now_ = now_ + Duration::FromMilliseconds(ms); }

  // Runs the subscription binding and notification-delivery coroutines.
  void Drain() { opcua::Drain(executor_); }

  // Creates a subscription and one monitored item on it, binds them, and
  // returns {subscription_id, backing client handle}. Notifications must be
  // pushed under the backing handle; the session republishes them under the
  // client's own.
  struct CreatedItem {
    SubscriptionId subscription_id = 0;
    MonitoredItemId monitored_item_id = 0;
    std::size_t backing_index = 0;
    UInt32 backing_client_handle = 0;
  };

  CreatedItem CreateSubscriptionWithItem(UInt32 client_handle,
                                         NumericId node_id) {
    const auto created = session_.CreateSubscription(
        {.parameters = {.publishing_interval_ms = 100,
                        .lifetime_count = 60,
                        .max_keep_alive_count = 3,
                        .publishing_enabled = true}});
    EXPECT_EQ(created.status.code(), StatusCode::Good);

    const auto items = session_.CreateMonitoredItems(
        {.subscription_id = created.subscription_id,
         .items_to_create = {
             {.item_to_monitor = {.node_id = NumericNode(node_id),
                                  .attribute_id = AttributeId::Value},
              .requested_parameters = {.client_handle = client_handle,
                                       .queue_size = 1,
                                       .discard_oldest = true}}}});
    EXPECT_EQ(items.results.size(), 1u);
    Drain();

    const std::size_t backing_index = backing_states_->size() - 1;
    EXPECT_EQ(backing(backing_index).added_items.size(), 1u);
    return {
        .subscription_id = created.subscription_id,
        .monitored_item_id = items.results[0].monitored_item_id,
        .backing_index = backing_index,
        .backing_client_handle = backing(backing_index).BackingClientHandle(0)};
  }

  void PushDataChange(const CreatedItem& item, double value) {
    backing(item.backing_index)
        .PushDataChange(item.backing_client_handle,
                        DataValue{Variant{value}, {}, now_, now_});
    Drain();
  }

 private:
  DateTime now_;
  TestExecutor executor_;
  std::shared_ptr<BackingStates> backing_states_;
  ServerSession session_;
};

// The single data-change value in a PublishResponse, or nullopt when the
// response carries no data change (a keep-alive, for instance). Reads the
// vector only after checking it, since an empty notification_data indexed at
// [0] is out of bounds and std::get_if then reads a garbage variant index.
struct PublishedDataChange {
  UInt32 client_handle = 0;
  double value = 0;
};

std::optional<PublishedDataChange> SingleDataChange(
    const PublishResponse& response) {
  if (response.notification_message.notification_data.size() != 1)
    return std::nullopt;
  const auto* data_change = std::get_if<DataChangeNotification>(
      &response.notification_message.notification_data[0]);
  if (!data_change || data_change->monitored_items.size() != 1)
    return std::nullopt;
  double value = 0;
  if (!data_change->monitored_items[0].value.value.get(value))
    return std::nullopt;
  return PublishedDataChange{
      .client_handle = data_change->monitored_items[0].client_handle,
      .value = value};
}

ua::BrowseResponse TwoReferenceBrowseResponse() {
  return {.response_header = {.service_result = Status{StatusCode::Good}},
          .results = {
              {.status_code = Status{StatusCode::Good},
               .references = {{.reference_type_id = NumericNode(501),
                               .is_forward = true,
                               .node_id = ExpandedNodeId{NumericNode(601)}},
                              {.reference_type_id = NumericNode(502),
                               .is_forward = true,
                               .node_id = ExpandedNodeId{NumericNode(602)}}}}}};
}

TEST(ServerSessionTest, StoresContinuationPointsAndResumesBrowse) {
  SessionHarness harness{1001, ParseTime("2026-04-20 17:00:00")};

  // Two references at a page size of one: the first is returned, the second
  // held behind a continuation point.
  const auto paged =
      harness.session().StoreBrowseResults(TwoReferenceBrowseResponse(), 1);
  ASSERT_EQ(paged.results.size(), 1u);
  ASSERT_EQ(paged.results[0].references.size(), 1u);
  EXPECT_EQ(paged.results[0].references[0].node_id, NumericNode(601));
  ASSERT_FALSE(paged.results[0].continuation_point.empty());

  const auto next = harness.session().BrowseNext(
      {.continuation_points = {paged.results[0].continuation_point}});
  ASSERT_EQ(next.results.size(), 1u);
  EXPECT_EQ(next.results[0].status_code, StatusCode::Good);
  ASSERT_EQ(next.results[0].references.size(), 1u);
  EXPECT_EQ(next.results[0].references[0].node_id, NumericNode(602));

  // The point is consumed: resuming it again finds nothing to resume.
  const auto reused = harness.session().BrowseNext(
      {.continuation_points = {paged.results[0].continuation_point}});
  ASSERT_EQ(reused.results.size(), 1u);
  EXPECT_EQ(reused.results[0].status_code,
            StatusCode::Bad_ContinuationPointInvalid);
}

TEST(ServerSessionTest, RejectsBrowseWhenContinuationPointLimitReached) {
  SessionHarness harness{1001, ParseTime("2026-04-20 17:00:00")};

  const auto page_once = [&] {
    return harness.session().StoreBrowseResults(TwoReferenceBrowseResponse(),
                                                1);
  };

  ByteString first_continuation_point;
  for (std::uint32_t i = 0; i < kMaxBrowseContinuationPoints; ++i) {
    const auto paged = page_once();
    ASSERT_EQ(paged.results[0].status_code, StatusCode::Good);
    ASSERT_FALSE(paged.results[0].continuation_point.empty());
    if (i == 0)
      first_continuation_point = paged.results[0].continuation_point;
  }

  // The next allocation is refused rather than growing without bound.
  const auto overflow = page_once();
  ASSERT_EQ(overflow.results.size(), 1u);
  EXPECT_EQ(overflow.results[0].status_code,
            StatusCode::Bad_NoContinuationPoints);
  EXPECT_TRUE(overflow.results[0].continuation_point.empty());

  // Releasing one frees a slot, so Browse can page again.
  const auto released = harness.session().BrowseNext(
      {.continuation_points = {first_continuation_point},
       .release_continuation_points = true});
  ASSERT_EQ(released.results.size(), 1u);
  EXPECT_EQ(released.results[0].status_code, StatusCode::Good);

  const auto after_release = page_once();
  EXPECT_EQ(after_release.results[0].status_code, StatusCode::Good);
  EXPECT_FALSE(after_release.results[0].continuation_point.empty());
}

// OPC UA Part 4 §5.13.7 TransferSubscriptions: the subscription moves to the
// receiving session, which then publishes its notifications.
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.7
TEST(ServerSessionTest, TransfersSubscriptionsAndPublishesQueuedData) {
  const auto start = ParseTime("2026-04-20 18:00:00");
  // One executor and one backing-state list, so the transferred subscription
  // keeps running against the same backing subscription it was created with.
  TestExecutor executor;
  auto backing_states = std::make_shared<BackingStates>();
  SessionHarness source{1101, start, executor, backing_states};
  SessionHarness target{1102, start, executor, backing_states};

  const auto item = source.CreateSubscriptionWithItem(/*client_handle=*/44,
                                                      /*node_id=*/301);

  const auto transferred = target.session().TransferSubscriptionsFrom(
      source.session(), {.subscription_ids = {item.subscription_id},
                         .send_initial_values = true});
  ASSERT_EQ(transferred.results.size(), 1u);
  EXPECT_TRUE(transferred.results[0].status_code.good());
  EXPECT_FALSE(source.session().HasSubscription(item.subscription_id));
  EXPECT_TRUE(target.session().HasSubscription(item.subscription_id));

  // The binding survived the move: a value pushed to the backing subscription
  // created under the SOURCE session is published by the TARGET.
  target.PushDataChange(item, 77.0);
  target.Advance(100);

  const auto published = target.session().Publish({});
  EXPECT_EQ(published.status.code(), StatusCode::Good);
  EXPECT_EQ(published.subscription_id, item.subscription_id);
  const auto data_change = SingleDataChange(published);
  ASSERT_TRUE(data_change.has_value());
  EXPECT_EQ(data_change->client_handle, 44u);
  EXPECT_EQ(data_change->value, 77.0);
}

// Publish arbitration is round-robin across a session's subscriptions: one
// subscription with data cannot hold the session's publish slot while another
// waits. Same fairness rule as the per-item one inside a subscription (see
// server_subscription_unittest.cpp), one level up.
TEST(ServerSessionTest, PublishRotatesAcrossSubscriptions) {
  SessionHarness harness{1301, ParseTime("2026-04-20 20:00:00")};

  const auto first = harness.CreateSubscriptionWithItem(/*client_handle=*/11,
                                                        /*node_id=*/401);
  const auto second = harness.CreateSubscriptionWithItem(/*client_handle=*/22,
                                                         /*node_id=*/402);
  ASSERT_NE(first.subscription_id, second.subscription_id);

  harness.PushDataChange(first, 1.0);
  harness.PushDataChange(second, 2.0);
  harness.Advance(100);

  const auto first_publish = harness.session().Publish({});
  const auto first_data = SingleDataChange(first_publish);
  ASSERT_TRUE(first_data.has_value());

  harness.Advance(100);
  const auto second_publish = harness.session().Publish({});
  const auto second_data = SingleDataChange(second_publish);
  ASSERT_TRUE(second_data.has_value());

  // Both subscriptions were served, in some order, rather than one twice.
  EXPECT_NE(first_publish.subscription_id, second_publish.subscription_id);
  EXPECT_NE(first_data->client_handle, second_data->client_handle);
}

TEST(ServerSessionTest, ModifiesSubscriptionsAndRoutesMonitoredItemOperations) {
  SessionHarness harness{1201, ParseTime("2026-04-20 19:00:00")};

  const auto created = harness.session().CreateSubscription(
      {.parameters = {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true}});

  const auto modified = harness.session().ModifySubscription(
      {.subscription_id = created.subscription_id,
       .parameters = {.publishing_interval_ms = 250,
                      .lifetime_count = 80,
                      .max_keep_alive_count = 5,
                      .publishing_enabled = true}});
  EXPECT_EQ(modified.status.code(), StatusCode::Good);
  EXPECT_DOUBLE_EQ(modified.revised_publishing_interval_ms, 250.0);
  EXPECT_EQ(modified.revised_lifetime_count, 80u);
  EXPECT_EQ(modified.revised_max_keep_alive_count, 5u);

  const auto created_items = harness.session().CreateMonitoredItems(
      {.subscription_id = created.subscription_id,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(401),
                                .attribute_id = AttributeId::Value},
            .requested_parameters = {.client_handle = 7,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(created_items.results.size(), 1u);
  ASSERT_EQ(created_items.results[0].status.code(), StatusCode::Good);
  const auto monitored_item_id = created_items.results[0].monitored_item_id;
  harness.Drain();

  const auto modified_items = harness.session().ModifyMonitoredItems(
      {.subscription_id = created.subscription_id,
       .items_to_modify = {
           {.monitored_item_id = monitored_item_id,
            .requested_parameters = {.client_handle = 8,
                                     .sampling_interval_ms = 50,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(modified_items.results.size(), 1u);
  EXPECT_EQ(modified_items.results[0].status.code(), StatusCode::Good);

  const auto monitoring_mode = harness.session().SetMonitoringMode(
      {.subscription_id = created.subscription_id,
       .monitoring_mode = ua::MonitoringMode::Sampling,
       .monitored_item_ids = {monitored_item_id}});
  ASSERT_EQ(monitoring_mode.results.size(), 1u);
  EXPECT_TRUE(monitoring_mode.results[0].good());

  const auto disable_publishing = harness.session().SetPublishingMode(
      {.publishing_enabled = false,
       .subscription_ids = {created.subscription_id}});
  ASSERT_EQ(disable_publishing.results.size(), 1u);
  EXPECT_TRUE(disable_publishing.results[0].good());

  const auto deleted_items = harness.session().DeleteMonitoredItems(
      {.subscription_id = created.subscription_id,
       .monitored_item_ids = {monitored_item_id}});
  ASSERT_EQ(deleted_items.results.size(), 1u);
  EXPECT_TRUE(deleted_items.results[0].good());

  const auto deleted_subscriptions = harness.session().DeleteSubscriptions(
      {.subscription_ids = {created.subscription_id}});
  ASSERT_EQ(deleted_subscriptions.results.size(), 1u);
  EXPECT_TRUE(deleted_subscriptions.results[0].good());
  EXPECT_FALSE(harness.session().HasSubscription(created.subscription_id));
}

// Operations naming a subscription this session does not hold are refused
// rather than routed. OPC UA Part 4 §5.13 MonitoredItem Service Set,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13
TEST(ServerSessionTest, RejectsOperationsOnUnknownSubscription) {
  SessionHarness harness{1401, ParseTime("2026-04-20 21:00:00")};
  constexpr SubscriptionId kUnknown = 4242;

  EXPECT_EQ(harness.session()
                .ModifySubscription({.subscription_id = kUnknown})
                .status.code(),
            StatusCode::Bad_SubscriptionIdInvalid);

  const auto items = harness.session().CreateMonitoredItems(
      {.subscription_id = kUnknown,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(401),
                                .attribute_id = AttributeId::Value}}}});
  EXPECT_EQ(items.status.code(), StatusCode::Bad_SubscriptionIdInvalid);

  const auto deleted =
      harness.session().DeleteSubscriptions({.subscription_ids = {kUnknown}});
  ASSERT_EQ(deleted.results.size(), 1u);
  EXPECT_EQ(deleted.results[0].code(), StatusCode::Bad_SubscriptionIdInvalid);
}

TEST(ServerSessionTest, PublishWithoutSubscriptionsReturnsBadNoSubscription) {
  SessionHarness harness{1001, ParseTime("2026-04-20 17:00:00")};

  // OPC UA Part 4 §5.13.5 Publish: a Publish for a session with no
  // subscriptions is answered with Bad_NoSubscription.
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  const auto published = harness.session().Publish({});
  EXPECT_EQ(published.status.code(), StatusCode::Bad_NoSubscription);
}

}  // namespace
}  // namespace opcua
