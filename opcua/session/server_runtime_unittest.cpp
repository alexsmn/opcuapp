#include "opcua/session/server_runtime.h"

#include "opcua/session/server_runtime_contract_test.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace opcua {
namespace {

using test::DirectRuntimeFixture;
using test::NumericNode;

// The shared runtime contract, run in process. The same contract functions are
// executed by the transport suites against their own fixtures, so a divergence
// between "the runtime does this" and "the runtime does this over a transport"
// shows up as one of these failing on one side only.
class ServerRuntimeTest : public testing::Test {
 protected:
  DirectRuntimeFixture fixture_;
};

TEST_F(ServerRuntimeTest, RoutesReadRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesReadRequestsThroughActivatedSessionUser(fixture_);
}

TEST_F(ServerRuntimeTest, RoutesWriteRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesWriteRequestsThroughActivatedSessionUser(fixture_);
}

TEST_F(ServerRuntimeTest, RoutesCallRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesCallRequestsThroughActivatedSessionUser(fixture_);
}

TEST_F(ServerRuntimeTest, ServiceRequestsWithoutActivatedSessionAreRejected) {
  test::ExpectServiceRequestsWithoutActivatedSessionAreRejected(fixture_);
}

TEST_F(ServerRuntimeTest, HistoryReadRawPreservesPayloadThroughSession) {
  test::ExpectHistoryReadRawPreservesPayloadThroughActivatedSession(fixture_);
}

TEST_F(ServerRuntimeTest, RejectsHistoryReadRawWithoutActivatedSession) {
  test::ExpectRejectsHistoryReadRawWithoutActivatedSession(fixture_);
}

TEST_F(ServerRuntimeTest, NodeManagementMutationsPreserveBatchResults) {
  test::ExpectNodeManagementMutationsPreserveBatchResults(fixture_);
}

TEST_F(ServerRuntimeTest, PreservesLiveSubscriptionStateAcrossDetachAndResume) {
  test::ExpectPreservesLiveSubscriptionStateAcrossDetachAndResume(fixture_);
}

TEST_F(ServerRuntimeTest, TransfersSubscriptionsAcrossSessions) {
  test::ExpectTransfersSubscriptionsAcrossSessions(fixture_);
}

TEST_F(ServerRuntimeTest, CloseSessionClearsAttachedState) {
  test::ExpectCloseSessionClearsAttachedState(fixture_);
}

TEST_F(ServerRuntimeTest, PublishReturnsKeepAliveWhenNoNotificationsAreQueued) {
  test::ExpectPublishReturnsKeepAliveWhenNoNotifications(fixture_);
}

TEST_F(ServerRuntimeTest, RepublishReplaysNotificationUntilAcknowledged) {
  test::ExpectRepublishReplaysNotificationUntilAcknowledged(fixture_);
}

// --- runtime-specific behaviour, with no transport counterpart ------------

// RegisterNodes is answered by the runtime itself, which may return the
// requested ids unchanged — but must return one per requested node.
// OPC UA Part 4 §5.8.5 RegisterNodes,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.5
TEST_F(ServerRuntimeTest, RegisterNodesEchoesRequestedNodeIds) {
  DirectRuntimeFixture::ConnectionState connection;
  fixture_.CreateAndActivate(connection);

  const auto response = fixture_.HandleResponse<ua::RegisterNodesResponse>(
      connection, ua::RegisterNodesRequest{
                      .nodes_to_register = {NumericNode(41), NumericNode(42)}});

  EXPECT_EQ(response.response_header.service_result.code(), StatusCode::Good);
  EXPECT_EQ(response.registered_node_ids,
            (std::vector<NodeId>{NumericNode(41), NumericNode(42)}));

  const auto unregistered =
      fixture_.HandleResponse<ua::UnregisterNodesResponse>(
          connection,
          ua::UnregisterNodesRequest{.nodes_to_unregister = {NumericNode(41)}});
  EXPECT_EQ(unregistered.response_header.service_result.code(),
            StatusCode::Good);
}

// A fixture that owns its runtime directly, for the two cases that need a
// non-default ServerRuntimeContext.
class ConfiguredRuntimeTest : public testing::Test {
 protected:
  ConnectionState Activate(ServerRuntime& runtime) {
    ConnectionState connection;
    const auto created = std::get<CreateSessionResponse>(WaitAwaitable(
        executor_,
        runtime.Handle(connection, RequestBody{CreateSessionRequest{}})));
    const auto activated = std::get<ActivateSessionResponse>(WaitAwaitable(
        executor_,
        runtime.Handle(connection,
                       RequestBody{ActivateSessionRequest{
                           .session_id = created.session_id,
                           .authentication_token = created.authentication_token,
                           .user_name = LocalizedText{u"operator"},
                           .password = LocalizedText{u"secret"}}})));
    EXPECT_EQ(activated.status.code(), StatusCode::Good);
    return connection;
  }

  DateTime now_ = test::ParseTime("2026-04-22 09:00:00");
  TestExecutor executor_;
  test::ScriptedServices services_;
  std::shared_ptr<test::BackingStates> backing_states_ =
      std::make_shared<test::BackingStates>();
  ServerSessionManager session_manager_{{
      .authenticator = MakeCoroutineAuthenticator(
          [](LocalizedText, LocalizedText) -> CoStatusOr<AuthenticationResult> {
            co_return AuthenticationResult{.user_id = NumericNode(700, 5),
                                           .multi_sessions = true};
          }),
      .now = [this] { return now_; },
  }};
};

// A Publish that arrives before anything is due is held rather than answered
// empty, and the wait is scheduled through `post_delayed_task` — which the
// test substitutes, so no wall-clock time passes and the wait is observable.
TEST_F(ConfiguredRuntimeTest, PublishDelayUsesInjectedSchedulerCallback) {
  std::vector<Duration> scheduled_delays;
  std::vector<std::function<void()>> scheduled_tasks;

  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .now = [this] { return now_; },
      .post_delayed_task =
          [&](Duration delay, std::function<void()> task) {
            scheduled_delays.push_back(delay);
            scheduled_tasks.push_back(std::move(task));
          },
  }};

  ConnectionState connection = Activate(runtime);

  const auto subscription = std::get<CreateSubscriptionResponse>(WaitAwaitable(
      executor_,
      runtime.Handle(connection,
                     RequestBody{CreateSubscriptionRequest{
                         .parameters = {.publishing_interval_ms = 100,
                                        .lifetime_count = 60,
                                        .max_keep_alive_count = 3,
                                        .publishing_enabled = true}}})));
  ASSERT_EQ(subscription.status.code(), StatusCode::Good);

  auto publish = StartAwaitable<ResponseBody>(
      executor_, runtime.Handle(connection, RequestBody{PublishRequest{}}));
  Drain(executor_);

  // Nothing is due yet: the runtime scheduled a wait instead of answering.
  ASSERT_FALSE(scheduled_tasks.empty());
  EXPECT_GT(scheduled_delays.front().InMilliseconds(), 0);
  EXPECT_FALSE(publish->done);

  // Firing the scheduled task with the clock advanced completes it.
  now_ = now_ + Duration::FromMilliseconds(100);
  scheduled_tasks.front()();
  const auto body = WaitResult(executor_, publish);
  const auto* response = std::get_if<PublishResponse>(&body);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->status.code(), StatusCode::Good);
  EXPECT_EQ(response->subscription_id, subscription.subscription_id);
}

// Operation limits are enforced before the request reaches the application.
// OPC UA Part 5 §12.4 OperationLimits,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/12.4
TEST_F(ConfiguredRuntimeTest, RejectsRequestsExceedingOperationLimits) {
  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .operation_limits = {.max_nodes_per_read = 1},
      .now = [this] { return now_; },
  }};

  ConnectionState connection = Activate(runtime);

  const auto body = WaitAwaitable(
      executor_,
      runtime.Handle(
          connection,
          RequestBody{ua::ReadRequest{
              .nodes_to_read = {
                  {.node_id = NumericNode(1),
                   .attribute_id = static_cast<UInt32>(AttributeId::Value)},
                  {.node_id = NumericNode(2),
                   .attribute_id =
                       static_cast<UInt32>(AttributeId::Value)}}}}));

  const auto* response = std::get_if<ua::ReadResponse>(&body);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->response_header.service_result.code(),
            StatusCode::Bad_TooManyOperations);
  // The application never saw the oversized batch.
  EXPECT_EQ(services_.read_count, 0);
}

}  // namespace
}  // namespace opcua
