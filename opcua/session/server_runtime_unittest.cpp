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
// OPC UA Part 4 §5.9.5 RegisterNodes,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.9.5
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

// A client that keeps issuing requests keeps its session, however long ago it
// activated. OPC UA Part 4 §5.7.2 CreateSession: the Server terminates a
// Session "if the Client fails to issue a Service request on the Session
// within the timeout period",
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.7.2
//
// The timeout used to run from ActivateSession alone, and pruning happens when
// another session is created, so on the demo a web session that published
// continuously died at the next health-probe connect and every later request
// failed Bad_SessionIdInvalid.
TEST_F(ConfiguredRuntimeTest, RequestsKeepTheSessionAlivePastItsTimeout) {
  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .now = [this] { return now_; },
  }};
  ConnectionState active = Activate(runtime);
  const auto request = [&] {
    return WaitAwaitable(
        executor_,
        runtime.Handle(active, RequestBody{ua::RegisterNodesRequest{}}));
  };
  // CreateSessionRequest{} asks for no timeout, so the session gets the
  // manager's default.
  const Duration timeout = ServerSessionManagerContext{}.default_timeout;

  // Two requests, each within the timeout of the one before, spanning more
  // than a whole timeout since activation.
  for (int i = 0; i < 2; ++i) {
    now_ = now_ + timeout - Duration::FromMinutes(1);
    ASSERT_TRUE(std::holds_alternative<ua::RegisterNodesResponse>(request()));
  }

  // Another client's CreateSession is what prunes expired sessions.
  ConnectionState other;
  WaitAwaitable(executor_,
                runtime.Handle(other, RequestBody{CreateSessionRequest{}}));

  EXPECT_TRUE(std::holds_alternative<ua::RegisterNodesResponse>(request()));
}

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

// A Publish parked between polls finishes as soon as its connection is
// detached, without waiting out the scheduled delay. It used to sleep out its
// whole keep-alive -- and, once the delay's timer was torn down, never finish
// at all, which on Windows hung the io_context's destruction and with it the
// server's shutdown (opcuapp runs 36305338396 through 36328711074). The
// scheduled task is deliberately never fired here.
TEST_F(ConfiguredRuntimeTest, ParkedPublishFinishesWhenItsConnectionDetaches) {
  std::vector<std::function<void()>> scheduled_tasks;

  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .now = [this] { return now_; },
      .post_delayed_task =
          [&](Duration, std::function<void()> task) {
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
  ASSERT_FALSE(scheduled_tasks.empty());
  ASSERT_FALSE(publish->done) << "the Publish should be parked on its delay";

  // What a server does when the connection's read loop ends.
  connection.closed = true;
  runtime.Detach(connection);
  Drain(executor_);

  ASSERT_TRUE(publish->done)
      << "a parked Publish must not outlive its connection";
  const auto* response = std::get_if<PublishResponse>(&*publish->value);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->status.code(), StatusCode::Bad_SessionIdInvalid);
}

// Operation limits are enforced before the request reaches the application.
// OPC UA Part 5 §6.3.11 OperationLimitsType,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.3.11
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

// Server.ServerCapabilities.MaxByteStringLength is enforced, not just
// advertised: a Write value over it is refused for that node alone with
// Bad_OutOfRange, and the other nodes still reach the application.
// OPC UA Part 5 §6.3.2 ServerCapabilitiesType,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.3.2, and Part 4
// §5.11.4 Write,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.4
TEST_F(ConfiguredRuntimeTest, WriteRefusesByteStringsOverMaxByteStringLength) {
  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .operation_limits = {.max_byte_string_length = 4},
      .now = [this] { return now_; },
  }};

  ConnectionState connection = Activate(runtime);

  const auto value_of = [](Variant value) {
    return DataValue{std::move(value), {}, {}, {}};
  };
  const auto body = WaitAwaitable(
      executor_,
      runtime.Handle(
          connection,
          RequestBody{ua::WriteRequest{
              .nodes_to_write = {
                  {.node_id = NumericNode(1),
                   .attribute_id = static_cast<UInt32>(AttributeId::Value),
                   .value = value_of(ByteString{'a', 'b', 'c', 'd'})},
                  {.node_id = NumericNode(2),
                   .attribute_id = static_cast<UInt32>(AttributeId::Value),
                   .value = value_of(ByteString{'a', 'b', 'c', 'd', 'e'})},
                  {.node_id = NumericNode(3),
                   .attribute_id = static_cast<UInt32>(AttributeId::Value),
                   .value = value_of(std::vector<ByteString>{
                       ByteString{'a'}, ByteString(5, 'x')})}}}}));

  const auto* response = std::get_if<ua::WriteResponse>(&body);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->response_header.service_result.code(), StatusCode::Good);
  ASSERT_EQ(response->results.size(), 3u);
  EXPECT_EQ(response->results[0].code(), StatusCode::Good);
  EXPECT_EQ(response->results[1].code(), StatusCode::Bad_OutOfRange);
  EXPECT_EQ(response->results[2].code(), StatusCode::Bad_OutOfRange);
  // Only the value within the limit reached the application.
  ASSERT_EQ(services_.last_write_inputs.size(), 1u);
  EXPECT_EQ(services_.last_write_inputs[0].node_id, NumericNode(1));
}

// A Method input argument over MaxByteStringLength fails that call with
// Bad_InvalidArgument and names the argument in inputArgumentResults, without
// invoking the method. OPC UA Part 4 §5.12.2 Call,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.12.2
TEST_F(ConfiguredRuntimeTest, CallRefusesByteStringArgumentsOverTheLimit) {
  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .operation_limits = {.max_byte_string_length = 4},
      .now = [this] { return now_; },
  }};

  ConnectionState connection = Activate(runtime);

  const auto body = WaitAwaitable(
      executor_,
      runtime.Handle(
          connection,
          RequestBody{ua::CallRequest{
              .methods_to_call = {
                  {.object_id = NumericNode(1),
                   .method_id = NumericNode(2),
                   .input_arguments = {Variant{UInt32{7}},
                                       Variant{ByteString(5, 'x')}}},
                  {.object_id = NumericNode(1),
                   .method_id = NumericNode(2),
                   .input_arguments = {Variant{ByteString(4, 'x')}}}}}}));

  const auto* response = std::get_if<ua::CallResponse>(&body);
  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->results.size(), 2u);
  EXPECT_EQ(response->results[0].status_code.code(),
            StatusCode::Bad_InvalidArgument);
  ASSERT_EQ(response->results[0].input_argument_results.size(), 2u);
  EXPECT_EQ(response->results[0].input_argument_results[0].code(),
            StatusCode::Good);
  EXPECT_EQ(response->results[0].input_argument_results[1].code(),
            StatusCode::Bad_OutOfRange);
  EXPECT_EQ(response->results[1].status_code.code(), StatusCode::Good);
  // The refused call never reached the method; the other one did.
  EXPECT_EQ(services_.call_count, 1);
}

// Zero is "no limit" — the default a caller that configures nothing gets.
TEST_F(ConfiguredRuntimeTest, ZeroMaxByteStringLengthImposesNoLimit) {
  ServerRuntime runtime{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .now = [this] { return now_; },
  }};

  ConnectionState connection = Activate(runtime);

  const auto body = WaitAwaitable(
      executor_,
      runtime.Handle(
          connection,
          RequestBody{ua::WriteRequest{
              .nodes_to_write = {
                  {.node_id = NumericNode(1),
                   .attribute_id = static_cast<UInt32>(AttributeId::Value),
                   .value = DataValue{
                       Variant{ByteString(1 << 20, 'x')}, {}, {}, {}}}}}}));

  const auto* response = std::get_if<ua::WriteResponse>(&body);
  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->results.size(), 1u);
  EXPECT_EQ(response->results[0].code(), StatusCode::Good);
}

}  // namespace
}  // namespace opcua
