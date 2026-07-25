#include "opcua/client/client_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/monitored/monitored_item.h"
#include "opcua/test/scripted_transport.h"
#include "opcua/types/data_value.h"
#include "opcua/types/variant.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace opcua {
namespace {

using opcua::test::AsString;
using opcua::test::BuildServiceResponseFrame;
using opcua::test::PrimeConnectAndOpen;
using opcua::test::ScriptedState;
using opcua::test::ScriptedTransportFactory;

// request_id 2 / 3 (request_handle 1 / 2) — CreateSession then
// ActivateSession, the first services a connect sends.
void PrimeSessionEstablishment(const std::shared_ptr<ScriptedState>& state) {
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSessionResponse{
          .status = StatusCode::Good,
          .session_id = NodeId{111},
          .authentication_token = NodeId{222},
          .server_nonce = ByteString{},
          .revised_timeout = Duration::FromSeconds(60),
      }})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/3, /*request_handle=*/2,
      ResponseBody{ActivateSessionResponse{.status = StatusCode::Good}})));
}

// After ActivateSession the session reads Server_NamespaceArray: request_id 4 /
// request_handle 3.
void PrimeNamespaceArray(const std::shared_ptr<ScriptedState>& state) {
  DataValue value;
  value.value = Variant{std::vector<std::string>{
      "http://opcfoundation.org/UA/", "http://telecontrol.ru/opcua/scada"}};
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{ua::ReadResponse{
          .response_header = {.service_result = StatusCode::Good},
          .results = {std::move(value)}}})));
}

// Subscription creation follows the namespace-array read: request_id 5 /
// request_handle 4. The script deliberately stops here — every
// CreateMonitoredItems after it fails on the exhausted transport, which is how
// these tests get one notification per item without having to predict request
// ids (the publish loop starts issuing Publish as soon as the subscription
// exists, so ids past this point are not stable).
void PrimeSubscriptionCreation(const std::shared_ptr<ScriptedState>& state) {
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/5, /*request_handle=*/4,
      ResponseBody{
          CreateSubscriptionResponse{.status = StatusCode::Good,
                                     .subscription_id = 77,
                                     .revised_publishing_interval_ms = 500.0,
                                     .revised_lifetime_count = 1200,
                                     .revised_max_keep_alive_count = 20}})));
}

MonitoredItemCreateRequest MakeItemRequest(std::uint32_t node_id,
                                           std::uint32_t client_handle) {
  return MonitoredItemCreateRequest{
      .item_to_monitor = {.node_id = NodeId{node_id},
                          .attribute_id = AttributeId::Value},
      .requested_parameters = {.client_handle = client_handle,
                               .sampling_interval_ms = 250,
                               .queue_size = 1}};
}

// A session serves several independent consumers over its single server-side
// subscription — an aggregating proxy runs a permanent downstream event tap
// alongside one pump per client subscription. Each consumer must see only its
// own items' notifications, and outlive its peers' closes.
class ClientSubscriptionViewTest : public ::testing::Test {
 protected:
  std::shared_ptr<ClientSession> ConnectSession() {
    PrimeConnectAndOpen(state_);
    PrimeSessionEstablishment(state_);
    PrimeNamespaceArray(state_);
    PrimeSubscriptionCreation(state_);
    auto session = std::make_shared<ClientSession>(executor_, factory_);
    WaitAwaitable(executor_, session->Connect({.host = "localhost:4840"}));
    return session;
  }

  // ReadNext(0) never waits: it returns the close status on a closed view and
  // an empty batch on an open one, so it probes liveness without parking a
  // coroutine.
  static bool IsOpen(MonitoredItemSubscription& view, TestExecutor& executor) {
    return WaitAwaitable(executor, view.ReadNext(0)).ok();
  }

  // The notifications already queued for `view`, without waiting. A ReadNext
  // that has to wait means the queue is empty — which is exactly the failure
  // these tests look for, and must be reported rather than hung on.
  std::vector<ItemNotification> DrainNow(MonitoredItemSubscription& view) {
    auto pending = StartAwaitable(executor_, view.ReadNext(16));
    Drain(executor_);
    if (!pending->done)
      return {};
    auto batch = std::move(*pending->value);
    if (!batch.ok())
      return {};
    return std::move(*batch);
  }

  TestExecutor executor_;
  std::shared_ptr<ScriptedState> state_ = std::make_shared<ScriptedState>();
  ScriptedTransportFactory factory_{state_};
};

// The regression: while the notification queue was shared, whichever consumer
// happened to be parked in ReadNext drained the others' notifications. The
// proxy's event tap waits permanently, so it swallowed every aggregated data
// change and dropped it (it forwards only events) — proxy clients saw no live
// values at all. Here each view adds one item, both creates fail, and each view
// must see exactly its own item's notification; with one shared queue the first
// reader took both and the second got nothing.
TEST_F(ClientSubscriptionViewTest, ViewsDoNotDrainEachOther) {
  auto session = ConnectSession();

  auto first = session->CreateSubscription({}, {});
  ASSERT_TRUE(first.ok());
  auto second = session->CreateSubscription({}, {});
  ASSERT_TRUE(second.ok());

  WaitAwaitable(executor_, (*first)->AddItems({MakeItemRequest(1, 7)}));
  WaitAwaitable(executor_, (*second)->AddItems({MakeItemRequest(2, 9)}));
  Drain(executor_);

  const auto first_read = DrainNow(**first);
  ASSERT_EQ(first_read.size(), 1u) << "view drained another view's queue";
  const auto* first_notification =
      std::get_if<MonitoredItemNotification>(&first_read.front());
  ASSERT_NE(first_notification, nullptr);
  EXPECT_EQ(first_notification->client_handle, 7u);

  const auto second_read = DrainNow(**second);
  ASSERT_EQ(second_read.size(), 1u) << "view was starved by its peer";
  const auto* second_notification =
      std::get_if<MonitoredItemNotification>(&second_read.front());
  ASSERT_NE(second_notification, nullptr);
  EXPECT_EQ(second_notification->client_handle, 9u);
}

// Client handles are only unique within a consumer — the proxy's event tap
// hardcodes handle 1, and a data pump numbers its own items from 1 too. The
// owning view, not the handle, has to decide where a notification lands.
TEST_F(ClientSubscriptionViewTest, CollidingClientHandlesStayAttributed) {
  auto session = ConnectSession();

  auto first = session->CreateSubscription({}, {});
  ASSERT_TRUE(first.ok());
  auto second = session->CreateSubscription({}, {});
  ASSERT_TRUE(second.ok());

  WaitAwaitable(executor_, (*first)->AddItems({MakeItemRequest(1, 1)}));
  WaitAwaitable(executor_, (*second)->AddItems({MakeItemRequest(2, 1)}));
  Drain(executor_);

  EXPECT_EQ(DrainNow(**first).size(), 1u);
  EXPECT_EQ(DrainNow(**second).size(), 1u);
}

// Consumers share a server-side subscription but not a lifetime: one closing
// must not end another's stream.
TEST_F(ClientSubscriptionViewTest, ClosingOneViewLeavesTheOtherOpen) {
  auto session = ConnectSession();

  auto first = session->CreateSubscription({}, {});
  ASSERT_TRUE(first.ok());
  auto second = session->CreateSubscription({}, {});
  ASSERT_TRUE(second.ok());

  (*second)->Close(StatusCode::Bad_NoSubscription);
  Drain(executor_);

  EXPECT_TRUE(IsOpen(**first, executor_)) << "view was closed with its peer";
  EXPECT_FALSE(IsOpen(**second, executor_));
  EXPECT_EQ(WaitAwaitable(executor_, (*second)->ReadNext(16)).status().code(),
            StatusCode::Bad_NoSubscription);
}

// MonitoredItemIds are unique across the whole shared subscription, so without
// scoping one consumer could delete another's item by id.
TEST_F(ClientSubscriptionViewTest, ViewCannotRemoveAnotherViewsItem) {
  auto session = ConnectSession();

  auto first = session->CreateSubscription({}, {});
  ASSERT_TRUE(first.ok());
  auto second = session->CreateSubscription({}, {});
  ASSERT_TRUE(second.ok());

  auto first_items =
      WaitAwaitable(executor_, (*first)->AddItems({MakeItemRequest(1, 1)}));
  ASSERT_EQ(first_items.size(), 1u);
  const MonitoredItemId item_id = first_items.front().monitored_item_id;

  const auto refused =
      WaitAwaitable(executor_, (*second)->RemoveItems({&item_id, 1}));
  ASSERT_EQ(refused.size(), 1u);
  EXPECT_EQ(refused.front().code(), StatusCode::Bad_MonitoredItemIdInvalid);

  const auto accepted =
      WaitAwaitable(executor_, (*first)->RemoveItems({&item_id, 1}));
  ASSERT_EQ(accepted.size(), 1u);
  EXPECT_TRUE(accepted.front().good());
}

}  // namespace
}  // namespace opcua
