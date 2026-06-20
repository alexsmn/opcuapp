#include "opcua/binary/runtime.h"

#include "opcua/server_runtime_contract_test.h"

#include <memory>

#include <gtest/gtest.h>

namespace opcua::binary {
namespace {

template <typename T>
std::shared_ptr<T> UnownedService(T& service) {
  return std::shared_ptr<T>{&service, [](T*) {}};
}

DataServices MakeRuntimeDataServices(
    std::shared_ptr<test::TestCoroutineServices> coroutine_services,
    scada::MonitoredItemService& monitored_item_service) {
  return {.view_service_ = coroutine_services,
          .node_management_service_ = coroutine_services,
          .history_service_ = coroutine_services,
          .attribute_service_ = coroutine_services,
          .method_service_ = coroutine_services,
          .monitored_item_service_ = UnownedService(monitored_item_service)};
}

DataServices MakeCallbackRuntimeDataServices(
    scada::MonitoredItemService& monitored_item_service,
    scada::AttributeService& attribute_service,
    scada::ViewService& view_service,
    scada::HistoryService& history_service,
    scada::MethodService& method_service,
    scada::NodeManagementService& node_management_service) {
  return {.view_service_ = UnownedService(view_service),
          .node_management_service_ = UnownedService(node_management_service),
          .history_service_ = UnownedService(history_service),
          .attribute_service_ = UnownedService(attribute_service),
          .method_service_ = UnownedService(method_service),
          .monitored_item_service_ = UnownedService(monitored_item_service)};
}

std::vector<EndpointDescription> MakeTestEndpoints() {
  return {EndpointDescription{
      .endpoint_url = "opc.tcp://localhost:4840",
      .server =
          ApplicationDescription{
              .application_uri = "urn:test:server",
              .product_uri = "urn:test:product",
              .application_name = scada::LocalizedText{u"Test Server"},
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

class RuntimeTest : public testing::Test,
                    public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    return WaitAwaitable(
        executor_, runtime_.Handle<Response>(connection, std::move(request)));
  }

  std::pair<scada::NodeId, scada::NodeId> CreateAndActivate(
      ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    EXPECT_EQ(created.status.code(), scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = scada::LocalizedText{u"operator"},
                        .password = scada::LocalizedText{u"secret"},
                    });
    EXPECT_EQ(activated.status.code(), scada::StatusCode::Good);
    EXPECT_FALSE(activated.resumed);
    return {created.session_id, created.authentication_token};
  }

  void Detach(ConnectionState& connection) { runtime_.Detach(connection); }

  scada::StatusCode ReadStatus(ConnectionState& connection,
                               ReadRequest request) {
    return HandleResponse<ReadResponse>(connection, std::move(request))
        .status.code();
  }

  scada::StatusCode HistoryReadRawStatus(ConnectionState& connection,
                                         HistoryReadRawRequest request) {
    return HandleResponse<HistoryReadRawResponse>(connection,
                                                  std::move(request))
        .result.status.code();
  }

  scada::StatusCode HistoryReadEventsStatus(ConnectionState& connection,
                                            HistoryReadEventsRequest request) {
    return HandleResponse<HistoryReadEventsResponse>(connection,
                                                     std::move(request))
        .result.status.code();
  }

  Runtime runtime_{RuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = attribute_service_,
      .view_service = view_service_,
      .history_service = history_service_,
      .method_service = method_service_,
      .node_management_service = node_management_service_,
      .endpoints = MakeTestEndpoints(),
      .now = [this] { return now_; },
  }};
};

TEST_F(RuntimeTest, DiscoveryRequestsDoNotRequireActivatedSession) {
  ConnectionState connection;

  const auto endpoints = HandleResponse<GetEndpointsResponse>(
      connection, GetEndpointsRequest{.endpoint_url = "opc.tcp://client:4840"});
  ASSERT_EQ(endpoints.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(endpoints.endpoints.size(), 1u);
  EXPECT_EQ(endpoints.endpoints[0].endpoint_url, "opc.tcp://client:4840");
  EXPECT_EQ(endpoints.endpoints[0].security_mode, MessageSecurityMode::None);
  EXPECT_EQ(endpoints.endpoints[0].server.application_uri, "urn:test:server");

  const auto servers =
      HandleResponse<FindServersResponse>(connection, FindServersRequest{});
  ASSERT_EQ(servers.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(servers.servers.size(), 1u);
  EXPECT_EQ(servers.servers[0].application_uri, "urn:test:server");
}

TEST_F(RuntimeTest, RoutesReadRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesReadRequestsThroughActivatedSessionUser(*this);
}

TEST_F(RuntimeTest, ContextRoutesReadThroughNormalizedDataServices) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(908),
                  .attribute_id = scada::AttributeId::Value}}};
  EXPECT_CALL(attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::ServiceContext context,
              std::shared_ptr<const std::vector<scada::ReadValueId>> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_THAT(*inputs, testing::ElementsAre(request.inputs[0]));
            co_return std::vector{
                scada::DataValue{scada::Variant{908.0}, {}, now_, now_}};
          }));

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, scada::Variant{908.0});
}

TEST_F(RuntimeTest, RoutesWriteRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesWriteRequestsThroughActivatedSessionUser(*this);
}

TEST_F(RuntimeTest, RoutesCallRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesCallRequestsThroughActivatedSessionUser(*this);
}

TEST_F(RuntimeTest, PreservesLiveSubscriptionStateAcrossDetachAndResume) {
  test::ExpectPreservesLiveSubscriptionStateAcrossDetachAndResume(*this);
}

TEST_F(RuntimeTest, TransfersSubscriptionsAcrossSessions) {
  test::ExpectTransfersSubscriptionsAcrossSessions(*this);
}

TEST_F(RuntimeTest, CloseSessionClearsAttachedState) {
  test::ExpectCloseSessionClearsAttachedState(*this);
}

TEST_F(RuntimeTest, RejectsHistoryReadRawWithoutActivatedSession) {
  test::ExpectRejectsHistoryReadRawWithoutActivatedSession(*this);
}

TEST_F(RuntimeTest, HistoryReadRawPreservesPayloadThroughActivatedSession) {
  test::ExpectHistoryReadRawPreservesPayloadThroughActivatedSession(*this);
}

TEST_F(RuntimeTest, RejectsHistoryReadEventsWithoutActivatedSession) {
  test::ExpectRejectsHistoryReadEventsWithoutActivatedSession(*this);
}

TEST_F(RuntimeTest, HistoryReadEventsPreservesPayloadThroughActivatedSession) {
  test::ExpectHistoryReadEventsPreservesPayloadThroughActivatedSession(*this);
}

TEST_F(RuntimeTest, BrowseAndBrowseNextUseSessionScopedContinuationPoints) {
  test::ExpectBrowseAndBrowseNextUseSessionScopedContinuationPoints(*this);
}

TEST_F(RuntimeTest, NodeManagementMutationsPreserveBatchResults) {
  test::ExpectNodeManagementMutationsPreserveBatchResults(*this);
}

TEST_F(RuntimeTest, PublishReturnsKeepAliveWhenNoNotificationsAreQueued) {
  test::ExpectPublishReturnsKeepAliveWhenNoNotifications(*this);
}

TEST_F(RuntimeTest, RepublishReplaysNotificationUntilAcknowledged) {
  test::ExpectRepublishReplaysNotificationUntilAcknowledged(*this);
}

TEST_F(
    RuntimeTest,
    HandleDecodedRequestRejectsActivateSessionForUnknownAuthenticationToken) {
  ConnectionState connection;

  const auto response = WaitAwaitable(
      executor_, runtime_.HandleDecodedRequest(
                     connection,
                     DecodedRequest{
                         .header = {.authentication_token = {999u, 3},
                                    .request_handle = 7},
                         .body =
                             ActivateSessionRequest{
                                 .authentication_token = {999u, 3},
                                 .user_name = scada::LocalizedText{u"operator"},
                                 .password = scada::LocalizedText{u"secret"},
                                 .allow_anonymous = false,
                             },
                     }));

  EXPECT_FALSE(response.has_value());
}

TEST_F(RuntimeTest,
       HandleDecodedRequestRejectsAuthenticatedRequestWhenTokenIsNotAttached) {
  ConnectionState connection;
  const auto activated = CreateAndActivate(connection);
  const auto& authentication_token = activated.second;
  ASSERT_TRUE(connection.authentication_token.has_value());
  EXPECT_EQ(*connection.authentication_token, authentication_token);

  const auto response = WaitAwaitable(
      executor_,
      runtime_.HandleDecodedRequest(
          connection,
          DecodedRequest{
              .header = {.authentication_token = {998u, 3},
                         .request_handle = 8},
              .body = ReadRequest{.inputs = {{.node_id = test::NumericNode(9),
                                              .attribute_id =
                                                  scada::AttributeId::Value}}},
          }));

  ASSERT_TRUE(response.has_value());
  const auto* read = std::get_if<ReadResponse>(&*response);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->status.code(), scada::StatusCode::Bad_SessionIsLoggedOff);
  EXPECT_EQ(*connection.authentication_token, authentication_token);
}

TEST_F(RuntimeTest,
       AcceptsCanonicalRequestAndResponseTypesThroughAdapterSurface) {
  ConnectionState connection;
  CreateAndActivate(connection);

  EXPECT_CALL(attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::ServiceContext context,
              std::shared_ptr<const std::vector<scada::ReadValueId>> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_EQ(inputs->size(), 1u);
            if (inputs->size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            EXPECT_EQ((*inputs)[0].node_id, test::NumericNode(10));
            EXPECT_EQ((*inputs)[0].attribute_id, scada::AttributeId::Value);
            co_return std::vector{
                scada::DataValue{scada::Variant{99.0}, {}, now_, now_}};
          }));

  const auto response = HandleResponse<ReadResponse>(
      connection,
      ReadRequest{.inputs = {{.node_id = test::NumericNode(10),
                              .attribute_id = scada::AttributeId::Value}}});

  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, scada::Variant{99.0});
}

class CoroutineRuntimeTest : public testing::Test,
                             public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    return WaitAwaitable(
        executor_, runtime_.Handle<Response>(connection, std::move(request)));
  }

  void CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    ASSERT_EQ(created.status.code(), scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = scada::LocalizedText{u"operator"},
                        .password = scada::LocalizedText{u"secret"},
                    });
    ASSERT_EQ(activated.status.code(), scada::StatusCode::Good);
  }

  test::TestCoroutineServices coroutine_services_;
  Runtime runtime_{RuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = coroutine_services_,
      .view_service = coroutine_services_,
      .history_service = coroutine_services_,
      .method_service = coroutine_services_,
      .node_management_service = coroutine_services_,
      .now = [this] { return now_; },
  }};
};

TEST_F(CoroutineRuntimeTest,
       RoutesReadThroughCoroutineServicesWithoutCallbackAdapters) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(902),
                  .attribute_id = scada::AttributeId::Value}}};

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, coroutine_services_.read_value);
  EXPECT_EQ(coroutine_services_.read_count, 1);
  EXPECT_EQ(coroutine_services_.last_read_context.user_id(), expected_user_id_);
  EXPECT_THAT(coroutine_services_.last_read_inputs,
              testing::ElementsAre(request.inputs[0]));
}

class DataServicesRuntimeTest : public testing::Test,
                                public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    return WaitAwaitable(
        executor_, runtime_.Handle<Response>(connection, std::move(request)));
  }

  void CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    ASSERT_EQ(created.status.code(), scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = scada::LocalizedText{u"operator"},
                        .password = scada::LocalizedText{u"secret"},
                    });
    ASSERT_EQ(activated.status.code(), scada::StatusCode::Good);
  }

  std::shared_ptr<test::TestCoroutineServices> coroutine_services_ =
      std::make_shared<test::TestCoroutineServices>();
  Runtime runtime_{DataServicesRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .data_services =
          MakeRuntimeDataServices(coroutine_services_, monitored_item_service_),
      .now = [this] { return now_; },
  }};
};

TEST_F(DataServicesRuntimeTest,
       RoutesReadThroughAggregateCoroutineSlotsWithoutCallbackAdapters) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(904),
                  .attribute_id = scada::AttributeId::Value}}};

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, coroutine_services_->read_value);
  EXPECT_EQ(coroutine_services_->read_count, 1);
  EXPECT_EQ(coroutine_services_->last_read_context.user_id(),
            expected_user_id_);
  EXPECT_THAT(coroutine_services_->last_read_inputs,
              testing::ElementsAre(request.inputs[0]));
}

class DataServicesCallbackRuntimeTest
    : public testing::Test,
      public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    return WaitAwaitable(
        executor_, runtime_.Handle<Response>(connection, std::move(request)));
  }

  void CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    ASSERT_EQ(created.status.code(), scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = scada::LocalizedText{u"operator"},
                        .password = scada::LocalizedText{u"secret"},
                    });
    ASSERT_EQ(activated.status.code(), scada::StatusCode::Good);
  }

  Runtime runtime_{DataServicesRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .data_services =
          MakeCallbackRuntimeDataServices(monitored_item_service_,
                                          attribute_service_,
                                          view_service_,
                                          history_service_,
                                          method_service_,
                                          node_management_service_),
      .now = [this] { return now_; },
  }};
};

TEST_F(DataServicesCallbackRuntimeTest,
       RoutesReadThroughAggregateUnownedSlots) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(906),
                  .attribute_id = scada::AttributeId::Value}}};
  EXPECT_CALL(attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::ServiceContext context,
              std::shared_ptr<const std::vector<scada::ReadValueId>> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_THAT(*inputs, testing::ElementsAre(request.inputs[0]));
            co_return std::vector{
                scada::DataValue{scada::Variant{906.0}, {}, now_, now_}};
          }));

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, scada::Variant{906.0});
}

}  // namespace
}  // namespace opcua::binary
