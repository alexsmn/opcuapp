#include "opcua/session/server_subscription.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/monitored/test/fake_monitored_item_subscription.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace opcua {
namespace {

constexpr SubscriptionId kSubscriptionId = 17;
// Client handles are the server's opaque correlation keys; a distinctive base
// keeps them apart from the backing handles the subscription allocates itself.
constexpr UInt32 kClientHandleBase = 1000;

NodeId NumericNode(NumericId id, NamespaceIndex ns = 2) {
  return {id, ns};
}

DateTime ParseTime(std::string_view value) {
  DateTime result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

// The client handles carried by the data changes in one PublishResponse, in
// wire order.
std::vector<UInt32> DataChangeHandles(const PublishResponse& publish) {
  std::vector<UInt32> handles;
  for (const auto& notification :
       publish.notification_message.notification_data) {
    const auto* data_change =
        std::get_if<DataChangeNotification>(&notification);
    if (!data_change)
      continue;
    for (const auto& item : data_change->monitored_items)
      handles.push_back(item.client_handle);
  }
  return handles;
}

// The values carried by the data changes in one PublishResponse, in wire
// order. Non-numeric and non-data-change notifications are skipped.
std::vector<double> DataChangeValues(const PublishResponse& publish) {
  std::vector<double> values;
  for (const auto& notification :
       publish.notification_message.notification_data) {
    const auto* data_change =
        std::get_if<DataChangeNotification>(&notification);
    if (!data_change)
      continue;
    for (const auto& item : data_change->monitored_items) {
      double value = 0;
      if (item.value.value.get(value))
        values.push_back(value);
    }
  }
  return values;
}

// A ServerSubscription wired to a FakeMonitoredItemSubscription, with the
// executor that drives its binding and notification coroutines.
class SubscriptionHarness {
 public:
  explicit SubscriptionHarness(SubscriptionParameters parameters,
                               DateTime start = {})
      : start_{start},
        subscription_{
            kSubscriptionId, parameters, executor_,
            FakeMonitoredItemSubscription::MakeCreateSubscription(executor_,
                                                                  backing_),
            start} {}

  ServerSubscription& subscription() { return subscription_; }
  FakeMonitoredItemSubscription::State& backing() { return *backing_; }
  DateTime start() const { return start_; }

  // Runs every coroutine continuation the subscription has posted: item
  // binding, and delivery of pushed notifications into the publish queue.
  void Drain() { opcua::Drain(executor_); }

  DateTime At(int64_t offset_ms) const {
    return start_ + Duration::FromMilliseconds(offset_ms);
  }

  // Creates `count` monitored items on distinct nodes, with client handles
  // kClientHandleBase + i, and binds them. Returns their client handles.
  std::vector<UInt32> CreateItems(
      std::size_t count,
      UInt32 queue_size,
      MonitoringMode monitoring_mode = MonitoringMode::Reporting) {
    CreateMonitoredItemsRequest request{.subscription_id = kSubscriptionId};
    std::vector<UInt32> client_handles;
    for (std::size_t i = 0; i < count; ++i) {
      const UInt32 client_handle = kClientHandleBase + static_cast<UInt32>(i);
      client_handles.push_back(client_handle);
      request.items_to_create.push_back(
          {.item_to_monitor = {.node_id =
                                   NumericNode(101 + static_cast<NumericId>(i)),
                               .attribute_id = AttributeId::Value},
           .monitoring_mode = monitoring_mode,
           .requested_parameters = {.client_handle = client_handle,
                                    .queue_size = queue_size,
                                    .discard_oldest = true}});
    }
    const auto response = subscription_.CreateMonitoredItems(request);
    EXPECT_EQ(response.results.size(), count);
    Drain();
    EXPECT_EQ(backing_->added_items.size(), count);
    return client_handles;
  }

  // The backing client handle allocated for the item created with
  // `client_handle`. Notifications must be pushed under the backing handle;
  // the subscription republishes them under the client's own.
  UInt32 BackingHandleFor(UInt32 client_handle) const {
    for (const auto& added : backing_->added_items) {
      // The item's node id encodes its creation index (see CreateItems), but
      // matching on the request's own client handle is enough: the backing
      // request carries the backing handle, and the items were created in
      // client-handle order.
      if (added.request.item_to_monitor.node_id ==
          NumericNode(101 + static_cast<NumericId>(client_handle -
                                                   kClientHandleBase))) {
        return added.request.requested_parameters.client_handle;
      }
    }
    ADD_FAILURE() << "no backing item for client handle " << client_handle;
    return 0;
  }

  // Pushes one data change per item, then delivers them.
  void PushToAll(const std::vector<UInt32>& client_handles, double value) {
    for (const auto client_handle : client_handles) {
      backing_->PushDataChange(BackingHandleFor(client_handle),
                               DataValue{Variant{value}, {}, start_, start_});
    }
    Drain();
  }

 private:
  TestExecutor executor_;
  std::shared_ptr<FakeMonitoredItemSubscription::State> backing_ =
      std::make_shared<FakeMonitoredItemSubscription::State>();
  DateTime start_;
  ServerSubscription subscription_;
};

SubscriptionParameters DefaultParameters() {
  return {.publishing_interval_ms = 100,
          .lifetime_count = 60,
          .max_keep_alive_count = 3,
          .publishing_enabled = true};
}

// The regression behind opcuapp d3e76de: TryPublish used to drain exactly one
// entry from the subscription-wide pending queue, so a subscription carried at
// most one notification per publishing interval no matter how many monitored
// items it had — and with the per-item queue limit trimming that same shared
// deque, a subset of items held the front while the rest never surfaced at all.
TEST(ServerSubscriptionTest, OnePublishCarriesEveryItemThatHasData) {
  constexpr std::size_t kItemCount = 18;
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto client_handles = harness.CreateItems(kItemCount, /*queue_size=*/1);
  harness.PushToAll(client_handles, 1.0);

  const auto publish = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(publish.has_value());
  const auto handles = DataChangeHandles(*publish);
  EXPECT_EQ(handles.size(), kItemCount);
  EXPECT_EQ(std::set<UInt32>(handles.begin(), handles.end()),
            std::set<UInt32>(client_handles.begin(), client_handles.end()));
  // Nothing was left behind, so the client is not told to publish again.
  EXPECT_FALSE(publish->more_notifications);
  EXPECT_FALSE(harness.subscription().HasPendingNotifications());
}

// The starvation the one-per-publish bug produced was permanent, not just
// slow: with queue_size 1 the same few items kept the front of the shared
// deque and the rest were trimmed away unheard, publish after publish.
TEST(ServerSubscriptionTest, NoItemIsStarvedAcrossPublishCycles) {
  constexpr std::size_t kItemCount = 18;
  constexpr int kRounds = 5;
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto client_handles = harness.CreateItems(kItemCount, /*queue_size=*/1);

  std::set<UInt32> heard;
  for (int round = 0; round < kRounds; ++round) {
    harness.PushToAll(client_handles, static_cast<double>(round));
    const auto publish =
        harness.subscription().TryPublish(harness.At((round + 1) * 100));
    ASSERT_TRUE(publish.has_value());
    const auto handles = DataChangeHandles(*publish);
    heard.insert(handles.begin(), handles.end());
  }

  EXPECT_EQ(heard,
            std::set<UInt32>(client_handles.begin(), client_handles.end()));
}

// OPC UA Part 4 §5.13.2 CreateSubscription: maxNotificationsPerPublish is the
// maximum number of notifications the Client wishes to receive in a single
// Publish response.
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2
TEST(ServerSubscriptionTest, HonoursMaxNotificationsPerPublish) {
  constexpr std::size_t kItemCount = 18;
  constexpr UInt32 kLimit = 5;
  auto parameters = DefaultParameters();
  parameters.max_notifications_per_publish = kLimit;
  SubscriptionHarness harness{parameters, ParseTime("2026-04-20 10:00:00")};

  const auto client_handles = harness.CreateItems(kItemCount, /*queue_size=*/1);
  harness.PushToAll(client_handles, 1.0);

  const auto first = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(DataChangeHandles(*first).size(), kLimit);
  // More is queued, and the client is told so.
  EXPECT_TRUE(first->more_notifications);

  // The remainder is delivered by the following publishes, and every item is
  // heard exactly once.
  std::vector<UInt32> heard = DataChangeHandles(*first);
  int publishes = 1;
  for (int i = 1; harness.subscription().HasPendingNotifications() && i < 10;
       ++i) {
    const auto publish =
        harness.subscription().TryPublish(harness.At((i + 1) * 100));
    ASSERT_TRUE(publish.has_value());
    const auto handles = DataChangeHandles(*publish);
    EXPECT_LE(handles.size(), kLimit);
    heard.insert(heard.end(), handles.begin(), handles.end());
    ++publishes;
  }

  EXPECT_EQ(publishes, 4);  // 5 + 5 + 5 + 3
  EXPECT_EQ(heard.size(), kItemCount);
  EXPECT_EQ(std::set<UInt32>(heard.begin(), heard.end()),
            std::set<UInt32>(client_handles.begin(), client_handles.end()));
}

// A zero maxNotificationsPerPublish means "no limit" — not "one". OPC UA Part 4
// §5.13.2 CreateSubscription,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2
//
// The server may still bound one response of its own accord (see
// kMaxNotificationsPerPublishResponse) and report the rest through
// moreNotifications; what a zero must never mean is a per-publish cap the
// client did not ask for. The batch here stays well under that server bound so
// the test pins the client-side rule alone.
TEST(ServerSubscriptionTest, ZeroMaxNotificationsPerPublishMeansUnlimited) {
  constexpr std::size_t kItemCount = 18;
  auto parameters = DefaultParameters();
  parameters.max_notifications_per_publish = 0;
  SubscriptionHarness harness{parameters, ParseTime("2026-04-20 10:00:00")};

  const auto client_handles = harness.CreateItems(kItemCount, /*queue_size=*/4);
  // Four values per item: 72 notifications, all of which one publish carries.
  for (int round = 0; round < 4; ++round)
    harness.PushToAll(client_handles, static_cast<double>(round));

  const auto publish = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(DataChangeHandles(*publish).size(), kItemCount * 4);
  EXPECT_FALSE(publish->more_notifications);
}

TEST(ServerSubscriptionTest, PublishesAcknowledgesAndRepublishesData) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto client_handles = harness.CreateItems(1, /*queue_size=*/2);
  harness.backing().PushDataChange(
      harness.BackingHandleFor(client_handles[0]),
      DataValue{Variant{12.5}, {}, harness.start(), harness.At(1000)});
  harness.Drain();

  const auto publish = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(publish->subscription_id, kSubscriptionId);
  EXPECT_EQ(publish->available_sequence_numbers, (std::vector<UInt32>{1u}));
  EXPECT_EQ(DataChangeValues(*publish), (std::vector<double>{12.5}));

  EXPECT_EQ(harness.subscription().Republish(1u).status.code(),
            StatusCode::Good);
  EXPECT_EQ(harness.subscription().Acknowledge(std::vector<UInt32>{1u}),
            (std::vector<StatusCode>{StatusCode::Good}));
  // Acknowledged messages leave the retransmission queue.
  EXPECT_EQ(harness.subscription().Republish(1u).status.code(),
            StatusCode::Bad_MessageNotAvailable);
}

TEST(ServerSubscriptionTest, AcknowledgeUnknownSequenceNumberReports) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  EXPECT_EQ(harness.subscription().Acknowledge(std::vector<UInt32>{999u}),
            (std::vector<StatusCode>{StatusCode::Bad_SequenceNumberUnknown}));
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

TEST(ServerSubscriptionTest,
     ModifiesParametersAndGeneratesAcknowledgedKeepAlive) {
  SubscriptionHarness harness{{.publishing_interval_ms = 100,
                               .lifetime_count = 60,
                               .max_keep_alive_count = 2,
                               .publishing_enabled = true},
                              ParseTime("2026-04-20 11:00:00")};

  const auto modified = harness.subscription().Modify(
      {.subscription_id = kSubscriptionId,
       .parameters = {.publishing_interval_ms = 250,
                      .lifetime_count = 80,
                      .max_keep_alive_count = 5,
                      .publishing_enabled = true}});
  EXPECT_EQ(modified.status.code(), StatusCode::Good);
  EXPECT_DOUBLE_EQ(modified.revised_publishing_interval_ms, 250.0);
  EXPECT_EQ(modified.revised_lifetime_count, 80u);
  EXPECT_EQ(modified.revised_max_keep_alive_count, 5u);

  const auto client_handles = harness.CreateItems(1, /*queue_size=*/1);

  // Nothing queued yet: the first message is an empty keep-alive.
  const auto first_keep_alive =
      harness.subscription().TryPublish(harness.At(250));
  ASSERT_TRUE(first_keep_alive.has_value());
  EXPECT_EQ(first_keep_alive->notification_message.sequence_number, 1u);
  EXPECT_TRUE(first_keep_alive->notification_message.notification_data.empty());

  // With publishing disabled the queued value is withheld and keep-alives
  // continue.
  harness.subscription().SetPublishingEnabled(false);
  harness.backing().PushDataChange(
      harness.BackingHandleFor(client_handles[0]),
      DataValue{Variant{77.0}, {}, harness.At(1000), harness.At(1000)});
  harness.Drain();

  const auto disabled_keep_alive =
      harness.subscription().TryPublish(harness.At(1510));
  ASSERT_TRUE(disabled_keep_alive.has_value());
  EXPECT_GT(disabled_keep_alive->notification_message.sequence_number, 0u);
  EXPECT_TRUE(
      disabled_keep_alive->notification_message.notification_data.empty());

  harness.subscription().SetPublishingEnabled(true);
  // Still inside the publishing interval.
  EXPECT_FALSE(harness.subscription().TryPublish(harness.At(1520)).has_value());

  const auto publish = harness.subscription().TryPublish(harness.At(1760));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(DataChangeValues(*publish), (std::vector<double>{77.0}));
}

TEST(ServerSubscriptionTest, BoundsRetransmissionQueue) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  constexpr std::size_t kOverflow = 5;
  const std::size_t total =
      ServerSubscription::kMaxRetransmitQueueNotifications + kOverflow;

  const auto client_handles = harness.CreateItems(1, /*queue_size=*/1);
  const UInt32 backing_handle = harness.BackingHandleFor(client_handles[0]);

  // One value per publish, so each publish produces its own retained message.
  std::optional<PublishResponse> last_publish;
  for (std::size_t i = 0; i < total; ++i) {
    harness.backing().PushDataChange(backing_handle,
                                     DataValue{Variant{static_cast<double>(i)},
                                               {},
                                               harness.start(),
                                               harness.start()});
    harness.Drain();
    last_publish = harness.subscription().TryPublish(
        harness.At(static_cast<int64_t>((i + 1) * 100)));
    ASSERT_TRUE(last_publish.has_value());
  }

  // The retransmission queue is capped: the oldest messages were dropped.
  EXPECT_EQ(last_publish->available_sequence_numbers.size(),
            ServerSubscription::kMaxRetransmitQueueNotifications);
  EXPECT_EQ(harness.subscription().Republish(1u).status.code(),
            StatusCode::Bad_MessageNotAvailable);
  EXPECT_EQ(harness.subscription()
                .Republish(static_cast<UInt32>(total))
                .status.code(),
            StatusCode::Good);
}

TEST(ServerSubscriptionTest, NonReportingMonitoringModeSuppressesDataChanges) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto client_handles =
      harness.CreateItems(1, /*queue_size=*/4, MonitoringMode::Sampling);
  harness.PushToAll(client_handles, 12.5);

  // Sampling (not Reporting) mode collects values but does not queue them for
  // publishing.
  EXPECT_FALSE(harness.subscription().HasPendingNotifications());
}

TEST(ServerSubscriptionTest, QueueOverflowCapsQueuedDataChanges) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto client_handles = harness.CreateItems(1, /*queue_size=*/2);
  const UInt32 backing_handle = harness.BackingHandleFor(client_handles[0]);

  // Three values into a queue of size two: the oldest is discarded.
  for (int i = 0; i < 3; ++i) {
    harness.backing().PushDataChange(backing_handle,
                                     DataValue{Variant{static_cast<double>(i)},
                                               {},
                                               harness.start(),
                                               harness.start()});
  }
  harness.Drain();

  const auto publish = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(DataChangeValues(*publish), (std::vector<double>{1.0, 2.0}));
}

TEST(ServerSubscriptionTest, AppliesAbsoluteDeadbandFilter) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto create = harness.subscription().CreateMonitoredItems(
      {.subscription_id = kSubscriptionId,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = AttributeId::Value},
            .requested_parameters = {
                .client_handle = kClientHandleBase,
                .filter = MonitoringFilter{DataChangeFilter{
                    .deadband_type = DeadbandType::Absolute,
                    .deadband_value = 5.0}},
                .queue_size = 10}}}});
  ASSERT_EQ(create.results.size(), 1u);
  harness.Drain();
  ASSERT_EQ(harness.backing().added_items.size(), 1u);

  // 10.0 reported (first); 12.0 within the 5.0 deadband (skipped); 16.0 differs
  // from 10.0 by 6.0 (reported).
  const UInt32 backing_handle = harness.BackingHandleFor(kClientHandleBase);
  for (const double value : {10.0, 12.0, 16.0}) {
    harness.backing().PushDataChange(
        backing_handle,
        DataValue{Variant{value}, {}, harness.start(), harness.start()});
  }
  harness.Drain();

  const auto publish = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(DataChangeValues(*publish), (std::vector<double>{10.0, 16.0}));
}

// The backing subscription outlives individual monitored items, so every path
// that stops using a binding must release it or the binding is held until the
// whole subscription closes.
TEST(ServerSubscriptionTest, DeleteMonitoredItemsReleasesBackingBinding) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  harness.CreateItems(2, /*queue_size=*/1);
  const MonitoredItemId backing_item_id =
      harness.backing().added_items[0].item_id;

  const auto response = harness.subscription().DeleteMonitoredItems(
      {.subscription_id = kSubscriptionId, .monitored_item_ids = {1}});
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].code(), StatusCode::Good);
  harness.Drain();

  EXPECT_EQ(harness.backing().removed_item_ids,
            (std::vector<MonitoredItemId>{backing_item_id}));
}

// A backing bind that fails must surface on the item rather than leaving it
// silently dead.
TEST(ServerSubscriptionTest, FailedBackingBindReportsItemStatus) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};
  harness.backing().add_status = Status{StatusCode::Bad_NodeIdUnknown};

  const auto create = harness.subscription().CreateMonitoredItems(
      {.subscription_id = kSubscriptionId,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = AttributeId::Value},
            .requested_parameters = {.client_handle = kClientHandleBase,
                                     .queue_size = 1}}}});
  ASSERT_EQ(create.results.size(), 1u);
  // The bind is asynchronous, so the create itself still succeeds.
  EXPECT_EQ(create.results[0].status.code(), StatusCode::Good);
  harness.Drain();

  const auto publish = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(publish.has_value());
  ASSERT_EQ(publish->notification_message.notification_data.size(), 1u);
  const auto* data_change = std::get_if<DataChangeNotification>(
      &publish->notification_message.notification_data[0]);
  ASSERT_NE(data_change, nullptr);
  ASSERT_EQ(data_change->monitored_items.size(), 1u);
  EXPECT_EQ(data_change->monitored_items[0].client_handle, kClientHandleBase);
  EXPECT_EQ(data_change->monitored_items[0].value.status_code,
            StatusCode::Bad_NodeIdUnknown);
}

TEST(ServerSubscriptionTest,
     CreateMonitoredItemsRejectsUnsupportedAttributeSynchronously) {
  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};

  const auto response = harness.subscription().CreateMonitoredItems(
      {.subscription_id = kSubscriptionId,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = AttributeId::Description},
            .requested_parameters = {.client_handle = kClientHandleBase,
                                     .queue_size = 1}}}});
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].status.code(),
            StatusCode::Bad_AttributeIdInvalid);
  harness.Drain();
  EXPECT_TRUE(harness.backing().added_items.empty());
}

// A client that sets no limit does not get an unbounded message. Nothing caps
// `pending_notifications_` as a whole — only per-item trimming — so a
// subscription with many items and a deep queue can hold arbitrarily many, and
// draining all of them into one response would build a message with no bound
// against transports that carry an explicit max_message_size. The server caps
// the response and reports the rest through moreNotifications, which is what
// that flag is for.
TEST(ServerSubscriptionTest, CapsOneResponseAtTheServerBound) {
  constexpr std::size_t kItemCount = 20;
  constexpr std::size_t kValuesPerItem = 60;  // 1200 notifications queued
  constexpr std::size_t kQueued = kItemCount * kValuesPerItem;
  static_assert(kQueued >
                ServerSubscription::kMaxNotificationsPerPublishResponse);

  auto parameters = DefaultParameters();
  parameters.max_notifications_per_publish = 0;  // no client-side limit
  SubscriptionHarness harness{parameters, ParseTime("2026-04-20 10:00:00")};

  const auto client_handles =
      harness.CreateItems(kItemCount, /*queue_size=*/kValuesPerItem);
  for (std::size_t round = 0; round < kValuesPerItem; ++round)
    harness.PushToAll(client_handles, static_cast<double>(round));

  const auto first = harness.subscription().TryPublish(harness.At(100));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(DataChangeHandles(*first).size(),
            ServerSubscription::kMaxNotificationsPerPublishResponse);
  EXPECT_TRUE(first->more_notifications);

  // The remainder is not dropped — it arrives on the next publish.
  const auto second = harness.subscription().TryPublish(harness.At(200));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(DataChangeHandles(*second).size(),
            kQueued - ServerSubscription::kMaxNotificationsPerPublishResponse);
  EXPECT_FALSE(second->more_notifications);
  EXPECT_FALSE(harness.subscription().HasPendingNotifications());
}

// Publishes `values_per_item` values for every item and delivers them as one
// message. Returns that publish.
std::optional<PublishResponse> PublishBatch(
    SubscriptionHarness& harness,
    const std::vector<UInt32>& client_handles,
    std::size_t values_per_item,
    int publish_index) {
  for (std::size_t round = 0; round < values_per_item; ++round)
    harness.PushToAll(client_handles, static_cast<double>(round));
  return harness.subscription().TryPublish(
      harness.At(static_cast<int64_t>(publish_index) * 100));
}

// The retransmission bound counts NOTIFICATIONS, not messages. That was the
// same thing only while a message carried exactly one notification — the very
// property TryPublish no longer has — so as a message count it would now
// retain 1024 messages of unbounded size. Three 500-notification messages
// exceed the bound and evict, where a message count would keep all three.
TEST(ServerSubscriptionTest,
     RetransmissionBoundCountsNotificationsNotMessages) {
  constexpr std::size_t kItemCount = 10;
  constexpr std::size_t kValuesPerItem = 50;  // 500 notifications per publish
  constexpr std::size_t kPerMessage = kItemCount * kValuesPerItem;
  static_assert(2 * kPerMessage <=
                ServerSubscription::kMaxRetransmitQueueNotifications);
  static_assert(3 * kPerMessage >
                ServerSubscription::kMaxRetransmitQueueNotifications);

  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};
  const auto client_handles =
      harness.CreateItems(kItemCount, /*queue_size=*/kValuesPerItem);

  ASSERT_TRUE(
      PublishBatch(harness, client_handles, kValuesPerItem, 1).has_value());
  const auto second = PublishBatch(harness, client_handles, kValuesPerItem, 2);
  ASSERT_TRUE(second.has_value());
  // Two messages, 1000 notifications: still inside the bound.
  EXPECT_EQ(second->available_sequence_numbers, (std::vector<UInt32>{1u, 2u}));

  const auto third = PublishBatch(harness, client_handles, kValuesPerItem, 3);
  ASSERT_TRUE(third.has_value());
  // 1500 would exceed it, so the oldest message goes — by notification count,
  // with only three messages held.
  EXPECT_EQ(third->available_sequence_numbers, (std::vector<UInt32>{2u, 3u}));
  EXPECT_EQ(harness.subscription().Republish(1u).status.code(),
            StatusCode::Bad_MessageNotAvailable);
  EXPECT_EQ(harness.subscription().Republish(2u).status.code(),
            StatusCode::Good);
}

// Acknowledge erases from the middle of the retransmission queue, so the
// running notification total has to follow it. If it does not, the total
// drifts upward and starts evicting messages the client still wants: here the
// third publish would push the drifted total past the bound and drop message 2,
// which is nobody's intent.
TEST(ServerSubscriptionTest, AcknowledgeReleasesRetainedNotifications) {
  constexpr std::size_t kItemCount = 10;
  constexpr std::size_t kValuesPerItem = 50;  // 500 notifications per publish

  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};
  const auto client_handles =
      harness.CreateItems(kItemCount, /*queue_size=*/kValuesPerItem);

  ASSERT_TRUE(
      PublishBatch(harness, client_handles, kValuesPerItem, 1).has_value());
  ASSERT_TRUE(
      PublishBatch(harness, client_handles, kValuesPerItem, 2).has_value());

  EXPECT_EQ(harness.subscription().Acknowledge(std::vector<UInt32>{1u}),
            (std::vector<StatusCode>{StatusCode::Good}));

  const auto third = PublishBatch(harness, client_handles, kValuesPerItem, 3);
  ASSERT_TRUE(third.has_value());
  // The acknowledged message's 500 notifications were released, so 2 and 3
  // together are inside the bound and both stay Republishable.
  EXPECT_EQ(third->available_sequence_numbers, (std::vector<UInt32>{2u, 3u}));
  EXPECT_EQ(harness.subscription().Republish(2u).status.code(),
            StatusCode::Good);
}

// The queue never evicts down to empty: whatever was just published stays
// Republishable even when the bound is already exceeded.
//
// The strictest form of that guard — a single message larger than the whole
// bound — cannot be produced through TryPublish while
// kMaxNotificationsPerPublishResponse (the most one response may carry) is at
// or below kMaxRetransmitQueueNotifications. The static_assert below is the
// tripwire: if the per-response cap is ever raised past the retransmit bound,
// it fires, and this test needs a case for a single oversized message.
TEST(ServerSubscriptionTest, RetransmissionQueueNeverEvictsToEmpty) {
  static_assert(ServerSubscription::kMaxNotificationsPerPublishResponse <=
                ServerSubscription::kMaxRetransmitQueueNotifications);

  constexpr std::size_t kItemCount = 20;
  constexpr std::size_t kValuesPerItem = 50;  // 1000 notifications per publish

  SubscriptionHarness harness{DefaultParameters(),
                              ParseTime("2026-04-20 10:00:00")};
  const auto client_handles =
      harness.CreateItems(kItemCount, /*queue_size=*/kValuesPerItem);

  ASSERT_TRUE(
      PublishBatch(harness, client_handles, kValuesPerItem, 1).has_value());
  const auto second = PublishBatch(harness, client_handles, kValuesPerItem, 2);
  ASSERT_TRUE(second.has_value());

  // 2000 retained exceeds the bound, so the older message goes — but the queue
  // stops there rather than emptying itself.
  EXPECT_EQ(second->available_sequence_numbers, (std::vector<UInt32>{2u}));
  EXPECT_EQ(harness.subscription().Republish(2u).status.code(),
            StatusCode::Good);
}

}  // namespace
}  // namespace opcua
