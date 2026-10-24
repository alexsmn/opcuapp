#include "opcua/session/server_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/monitored/test/fake_monitored_item_subscription.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

// SubscriptionLimits as ServerSession enforces them: refusal where OPC UA Part
// 4 defines a refusal code, revision where it defines revision (§5.13.2,
// §5.14.2; https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.2).

namespace opcua {
namespace {

using BackingStates =
    std::vector<std::shared_ptr<FakeMonitoredItemSubscription::State>>;

class ServerSessionLimitsTest : public testing::Test {
 protected:
  std::unique_ptr<ServerSession> MakeSession(SubscriptionLimits limits) {
    return std::make_unique<ServerSession>(ServerSessionContext{
        .session_id = NodeId{1, 2},
        .authentication_token = NodeId{1001, 3},
        .executor = executor_,
        .create_subscription =
            FakeMonitoredItemSubscription::MakeCreateSubscriptionPerCall(
                executor_, backing_states_),
        .subscription_limits = limits,
        .now = [this] { return now_; },
    });
  }

  static CreateSubscriptionRequest Subscription(double publishing_ms = 100) {
    return {.parameters = {.publishing_interval_ms = publishing_ms,
                           .lifetime_count = 60,
                           .max_keep_alive_count = 3,
                           .publishing_enabled = true}};
  }

  static CreateMonitoredItemsRequest Items(SubscriptionId subscription_id,
                                           std::size_t count,
                                           double sampling_ms = 100,
                                           UInt32 queue_size = 1) {
    CreateMonitoredItemsRequest request{.subscription_id = subscription_id};
    for (std::size_t i = 0; i < count; ++i) {
      request.items_to_create.push_back(
          {.item_to_monitor = {.node_id =
                                   NodeId{static_cast<NumericId>(i + 1), 2},
                               .attribute_id = AttributeId::Value},
           .requested_parameters = {.client_handle = static_cast<UInt32>(i + 1),
                                    .sampling_interval_ms = sampling_ms,
                                    .queue_size = queue_size}});
    }
    return request;
  }

  DateTime now_ = DateTime::Now();
  TestExecutor executor_;
  std::shared_ptr<BackingStates> backing_states_ =
      std::make_shared<BackingStates>();
};

TEST_F(ServerSessionLimitsTest, SubscriptionCapRefusesTheNextOne) {
  auto session = MakeSession({.max_subscriptions_per_session = 2});
  EXPECT_TRUE(session->CreateSubscription(Subscription()).status.good());
  EXPECT_TRUE(session->CreateSubscription(Subscription()).status.good());
  EXPECT_EQ(session->CreateSubscription(Subscription()).status.code(),
            StatusCode::Bad_TooManySubscriptions);
  EXPECT_EQ(session->GetSubscriptionIds().size(), 2u);
}

TEST_F(ServerSessionLimitsTest, PublishingIntervalIsRevisedUpToTheFloor) {
  auto session = MakeSession({.min_publishing_interval_ms = 1000});
  const auto fast = session->CreateSubscription(Subscription(50));
  EXPECT_EQ(fast.revised_publishing_interval_ms, 1000);
  const auto zero = session->CreateSubscription(Subscription(0));
  EXPECT_EQ(zero.revised_publishing_interval_ms, 1000);
  const auto slow = session->CreateSubscription(Subscription(5000));
  EXPECT_EQ(slow.revised_publishing_interval_ms, 5000);

  const auto modified =
      session->ModifySubscription({.subscription_id = slow.subscription_id,
                                   .parameters = Subscription(10).parameters});
  EXPECT_EQ(modified.revised_publishing_interval_ms, 1000);
}

TEST_F(ServerSessionLimitsTest, SamplingAndQueueAreRevised) {
  auto session =
      MakeSession({.min_sampling_interval_ms = 500, .max_queue_size = 10});
  const auto subscription = session->CreateSubscription(Subscription());
  const auto created = session->CreateMonitoredItems(
      Items(subscription.subscription_id, 1, /*sampling_ms=*/0,
            /*queue_size=*/1000));
  ASSERT_EQ(created.results.size(), 1u);
  EXPECT_EQ(created.results[0].revised_sampling_interval_ms, 500);
  EXPECT_EQ(created.results[0].revised_queue_size, 10u);
}

TEST_F(ServerSessionLimitsTest, ItemsPastTheSubscriptionQuotaAreRefused) {
  auto session = MakeSession({.max_monitored_items_per_subscription = 3});
  const auto subscription = session->CreateSubscription(Subscription());
  const auto created =
      session->CreateMonitoredItems(Items(subscription.subscription_id, 5));
  ASSERT_EQ(created.results.size(), 5u);
  for (int i = 0; i < 3; ++i)
    EXPECT_TRUE(created.results[i].status.good()) << i;
  for (int i = 3; i < 5; ++i) {
    EXPECT_EQ(created.results[i].status.code(),
              StatusCode::Bad_TooManyMonitoredItems)
        << i;
  }
  // A later request finds the subscription already full.
  const auto more =
      session->CreateMonitoredItems(Items(subscription.subscription_id, 1));
  ASSERT_EQ(more.results.size(), 1u);
  EXPECT_EQ(more.results[0].status.code(),
            StatusCode::Bad_TooManyMonitoredItems);
}

TEST_F(ServerSessionLimitsTest, SessionQuotaSpansSubscriptions) {
  auto session = MakeSession({.max_monitored_items_per_session = 4});
  const auto first = session->CreateSubscription(Subscription());
  const auto second = session->CreateSubscription(Subscription());
  const auto a = session->CreateMonitoredItems(Items(first.subscription_id, 3));
  const auto b =
      session->CreateMonitoredItems(Items(second.subscription_id, 3));
  ASSERT_EQ(b.results.size(), 3u);
  EXPECT_TRUE(b.results[0].status.good());
  EXPECT_EQ(b.results[1].status.code(), StatusCode::Bad_TooManyMonitoredItems);
  EXPECT_EQ(b.results[2].status.code(), StatusCode::Bad_TooManyMonitoredItems);
}

TEST_F(ServerSessionLimitsTest, DefaultsLeaveEverythingUnlimited) {
  auto session = MakeSession({});
  const auto subscription = session->CreateSubscription(Subscription(1));
  EXPECT_EQ(subscription.revised_publishing_interval_ms, 1);
  const auto created = session->CreateMonitoredItems(
      Items(subscription.subscription_id, 50, /*sampling_ms=*/0,
            /*queue_size=*/500));
  ASSERT_EQ(created.results.size(), 50u);
  EXPECT_TRUE(created.results[49].status.good());
  EXPECT_EQ(created.results[0].revised_queue_size, 500u);
}

}  // namespace
}  // namespace opcua
