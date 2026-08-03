#include "opcua/transport/binary/runtime.h"

#include "opcua/session/server_runtime_contract_test.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace opcua::binary {
namespace {

using test::NumericNode;

std::vector<EndpointDescription> MakeTestEndpoints() {
  return {EndpointDescription{
      .endpoint_url = "opc.tcp://localhost:4840",
      .server =
          ApplicationDescription{
              .application_uri = "urn:test:server",
              .product_uri = "urn:test:product",
              .application_name = LocalizedText{u"Test Server"},
              .application_type = ApplicationType::Server,
              .discovery_urls = {"opc.tcp://localhost:4840"},
          },
      .security_mode = MessageSecurityMode::None,
      .security_policy_uri = "http://opcfoundation.org/UA/SecurityPolicy#None",
      .user_identity_tokens =
          {UserTokenPolicy{.policy_id = "anonymous",
                           .token_type = UserTokenType::Anonymous},
           UserTokenPolicy{
               .policy_id = "username",
               .token_type = UserTokenType::UserName,
               .security_policy_uri =
                   "http://opcfoundation.org/UA/SecurityPolicy#None"}},
      .transport_profile_uri =
          "http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary",
  }};
}

// The UA Binary implementation of the shared server-runtime contract: the same
// contract functions session/server_runtime_unittest.cpp runs in process, run
// here through binary::Runtime's typed Handle<Response> surface instead. A
// behaviour the runtime has in process but loses behind the Binary transport —
// or vice versa — shows up as one of these failing on one side only.
class BinaryRuntimeFixture {
 public:
  using ConnectionState = opcua::binary::ConnectionState;

  struct SessionIds {
    NodeId session_id;
    NodeId authentication_token;
  };

  DateTime now_ = test::ParseTime("2026-04-22 09:00:00");
  const NodeId expected_user_id_ = NumericNode(700, 5);
  TestExecutor executor_;
  test::ScriptedServices services_;
  std::shared_ptr<test::BackingStates> backing_states_ =
      std::make_shared<test::BackingStates>();

  ServerSessionManager session_manager_{{
      .authenticator = MakeCoroutineAuthenticator(
          [this](LocalizedText user_name,
                 LocalizedText password) -> CoStatusOr<AuthenticationResult> {
            EXPECT_EQ(user_name, LocalizedText{u"operator"});
            EXPECT_EQ(password, LocalizedText{u"secret"});
            co_return AuthenticationResult{.user_id = expected_user_id_,
                                           .multi_sessions = true};
          }),
      .now = [this] { return now_; },
  }};

  // See DirectRuntimeFixture: deferred work is captured rather than scheduled,
  // because TestExecutor has no timer reactor and a real steady_timer would
  // never fire.
  std::vector<std::pair<Duration, std::function<void()>>> delayed_tasks_;

  Runtime runtime_{RuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .endpoints = MakeTestEndpoints(),
      .now = [this] { return now_; },
      .post_delayed_task =
          [this](Duration delay, std::function<void()> task) {
            delayed_tasks_.emplace_back(delay, std::move(task));
          },
  }};

  template <class Response, class Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    auto result = StartAwaitable<Response>(
        executor_, runtime_.Handle<Response>(connection, std::move(request)));

    for (int step = 0; step < 16 && !result->done; ++step) {
      opcua::Drain(executor_);
      if (result->done || delayed_tasks_.empty())
        break;
      auto [delay, task] = std::move(delayed_tasks_.front());
      delayed_tasks_.erase(delayed_tasks_.begin());
      now_ = now_ + delay;
      task();
    }
    opcua::Drain(executor_);

    EXPECT_TRUE(result->done) << "request never completed";
    if (!result->done || !result->value.has_value())
      return Response{};
    return std::move(*result->value);
  }

  SessionIds CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    EXPECT_EQ(created.status.code(), StatusCode::Good);
    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = LocalizedText{u"operator"},
                        .password = LocalizedText{u"secret"},
                    });
    EXPECT_EQ(activated.status.code(), StatusCode::Good);
    EXPECT_FALSE(activated.resumed);
    return {created.session_id, created.authentication_token};
  }

  void Detach(ConnectionState& connection) { runtime_.Detach(connection); }

  void Drain() { opcua::Drain(executor_); }

  void Advance(int64_t ms) { now_ = now_ + Duration::FromMilliseconds(ms); }

  FakeMonitoredItemSubscription::State& backing(std::size_t index) const {
    return *backing_states_->at(index);
  }
};

class BinaryRuntimeTest : public testing::Test {
 protected:
  BinaryRuntimeFixture fixture_;
};

// --- the shared contract, over UA Binary ---

TEST_F(BinaryRuntimeTest, RoutesReadRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesReadRequestsThroughActivatedSessionUser(fixture_);
}

TEST_F(BinaryRuntimeTest, RoutesWriteRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesWriteRequestsThroughActivatedSessionUser(fixture_);
}

TEST_F(BinaryRuntimeTest, RoutesCallRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesCallRequestsThroughActivatedSessionUser(fixture_);
}

TEST_F(BinaryRuntimeTest, ServiceRequestsWithoutActivatedSessionAreRejected) {
  test::ExpectServiceRequestsWithoutActivatedSessionAreRejected(fixture_);
}

TEST_F(BinaryRuntimeTest, HistoryReadRawPreservesPayloadThroughSession) {
  test::ExpectHistoryReadRawPreservesPayloadThroughActivatedSession(fixture_);
}

TEST_F(BinaryRuntimeTest, RejectsHistoryReadRawWithoutActivatedSession) {
  test::ExpectRejectsHistoryReadRawWithoutActivatedSession(fixture_);
}

TEST_F(BinaryRuntimeTest, NodeManagementMutationsPreserveBatchResults) {
  test::ExpectNodeManagementMutationsPreserveBatchResults(fixture_);
}

TEST_F(BinaryRuntimeTest, PreservesLiveSubscriptionStateAcrossDetachAndResume) {
  test::ExpectPreservesLiveSubscriptionStateAcrossDetachAndResume(fixture_);
}

TEST_F(BinaryRuntimeTest, TransfersSubscriptionsAcrossSessions) {
  test::ExpectTransfersSubscriptionsAcrossSessions(fixture_);
}

TEST_F(BinaryRuntimeTest, CloseSessionClearsAttachedState) {
  test::ExpectCloseSessionClearsAttachedState(fixture_);
}

TEST_F(BinaryRuntimeTest, PublishReturnsKeepAliveWhenNoNotificationsAreQueued) {
  test::ExpectPublishReturnsKeepAliveWhenNoNotifications(fixture_);
}

TEST_F(BinaryRuntimeTest, RepublishReplaysNotificationUntilAcknowledged) {
  test::ExpectRepublishReplaysNotificationUntilAcknowledged(fixture_);
}

// --- Binary-specific behaviour ---

TEST_F(BinaryRuntimeTest, DiscoveryRequestsDoNotRequireActivatedSession) {
  BinaryRuntimeFixture::ConnectionState connection;

  const auto endpoints = fixture_.HandleResponse<GetEndpointsResponse>(
      connection, GetEndpointsRequest{.endpoint_url = "opc.tcp://client:4840"});
  ASSERT_EQ(endpoints.status.code(), StatusCode::Good);
  ASSERT_EQ(endpoints.endpoints.size(), 1u);
  EXPECT_EQ(endpoints.endpoints[0].endpoint_url, "opc.tcp://client:4840");
  EXPECT_EQ(endpoints.endpoints[0].security_mode, MessageSecurityMode::None);
  EXPECT_EQ(endpoints.endpoints[0].server.application_uri, "urn:test:server");

  const auto servers = fixture_.HandleResponse<FindServersResponse>(
      connection, FindServersRequest{});
  ASSERT_EQ(servers.status.code(), StatusCode::Good);
  ASSERT_EQ(servers.servers.size(), 1u);
  EXPECT_EQ(servers.servers[0].application_uri, "urn:test:server");
}

// An ActivateSession naming an authentication token this runtime never issued
// is not answerable: there is no session to fault against, so the request is
// dropped rather than answered.
TEST_F(BinaryRuntimeTest,
       HandleDecodedRequestRejectsActivateSessionForUnknownToken) {
  BinaryRuntimeFixture::ConnectionState connection;

  const auto response = WaitAwaitable(
      fixture_.executor_,
      fixture_.runtime_.HandleDecodedRequest(
          connection, DecodedRequest{
                          .header = {.authentication_token = {999u, 3},
                                     .request_handle = 7},
                          .body =
                              ActivateSessionRequest{
                                  .authentication_token = {999u, 3},
                                  .user_name = LocalizedText{u"operator"},
                                  .password = LocalizedText{u"secret"},
                                  .allow_anonymous = false,
                              },
                      }));

  EXPECT_FALSE(response.has_value());
}

// A request whose header carries a token that is not the one attached to this
// connection must be refused with Bad_SessionIdInvalid — and the refusal has to
// reach the caller in the response's own status field. Answering a defaulted
// (Good, empty) response instead would present to a client as "the server has
// nothing for those nodes", which is indistinguishable from a legitimate empty
// read.
TEST_F(BinaryRuntimeTest,
       HandleDecodedRequestRejectsAuthenticatedRequestWhenTokenIsNotAttached) {
  BinaryRuntimeFixture::ConnectionState connection;
  const auto activated = fixture_.CreateAndActivate(connection);
  ASSERT_TRUE(connection.authentication_token.has_value());
  EXPECT_EQ(*connection.authentication_token, activated.authentication_token);

  const auto response = WaitAwaitable(
      fixture_.executor_,
      fixture_.runtime_.HandleDecodedRequest(
          connection,
          DecodedRequest{
              .header = {.authentication_token = {998u, 3},
                         .request_handle = 8},
              .body =
                  ua::ReadRequest{
                      .nodes_to_read = {{.node_id = NumericNode(9),
                                         .attribute_id = static_cast<UInt32>(
                                             AttributeId::Value)}}},
          }));

  ASSERT_TRUE(response.has_value());
  const auto* read = std::get_if<ua::ReadResponse>(&*response);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->response_header.service_result.code(),
            StatusCode::Bad_SessionIdInvalid);
  EXPECT_EQ(*connection.authentication_token, activated.authentication_token);
  // The rejected request must never have reached the application.
  EXPECT_EQ(fixture_.services_.read_count, 0);
}

// Operation limits configured on the Binary transport must actually bind. They
// were previously dropped when RuntimeContext built its inner
// ServerRuntimeContext, so the address space could advertise a limit the wire
// path did not enforce.
TEST(BinaryRuntimeLimitsTest, ConfiguredOperationLimitsAreEnforced) {
  class LimitedFixture : public BinaryRuntimeFixture {
   public:
    Runtime limited_runtime_{RuntimeContext{
        .executor = AnyExecutor{executor_},
        .session_manager = session_manager_,
        .callbacks =
            services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
        .endpoints = MakeTestEndpoints(),
        .operation_limits = {.max_nodes_per_read = 2},
        .now = [this] { return now_; },
    }};
  };

  LimitedFixture fixture;
  BinaryRuntimeFixture::ConnectionState connection;

  const auto created = WaitAwaitable(
      fixture.executor_, fixture.limited_runtime_.Handle<CreateSessionResponse>(
                             connection, CreateSessionRequest{}));
  ASSERT_EQ(created.status.code(), StatusCode::Good);
  const auto activated = WaitAwaitable(
      fixture.executor_,
      fixture.limited_runtime_.Handle<ActivateSessionResponse>(
          connection, ActivateSessionRequest{
                          .session_id = created.session_id,
                          .authentication_token = created.authentication_token,
                          .user_name = LocalizedText{u"operator"},
                          .password = LocalizedText{u"secret"},
                      }));
  ASSERT_EQ(activated.status.code(), StatusCode::Good);

  ua::ReadRequest request;
  for (NumericId id = 1; id <= 3; ++id) {
    request.nodes_to_read.push_back(
        {.node_id = NumericNode(id),
         .attribute_id = static_cast<UInt32>(AttributeId::Value)});
  }

  const auto response = WaitAwaitable(
      fixture.executor_, fixture.limited_runtime_.Handle<ua::ReadResponse>(
                             connection, std::move(request)));

  EXPECT_EQ(response.response_header.service_result.code(),
            StatusCode::Bad_TooManyOperations);
  EXPECT_EQ(fixture.services_.read_count, 0);
}

}  // namespace
}  // namespace opcua::binary
