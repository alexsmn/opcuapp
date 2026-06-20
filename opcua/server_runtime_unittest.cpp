#include "opcua/server_runtime.h"

#include "opcua/server_runtime_contract_test.h"

#include <functional>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace opcua {
namespace {

template <typename T>
std::shared_ptr<T> UnownedService(T& service) {
  return std::shared_ptr<T>{&service, [](T*) {}};
}

DataServices MakeRuntimeDataServices(
    std::shared_ptr<test::TestCoroutineServices> coroutine_services,
    opcua::scada::MonitoredItemService& monitored_item_service) {
  return {.view_service_ = coroutine_services,
          .node_management_service_ = coroutine_services,
          .history_service_ = coroutine_services,
          .attribute_service_ = coroutine_services,
          .method_service_ = coroutine_services,
          .monitored_item_service_ = UnownedService(monitored_item_service)};
}

DataServices MakeCallbackRuntimeDataServices(
    opcua::scada::MonitoredItemService& monitored_item_service,
    opcua::scada::AttributeService& attribute_service,
    opcua::scada::ViewService& view_service,
    opcua::scada::HistoryService& history_service,
    opcua::scada::MethodService& method_service,
    opcua::scada::NodeManagementService& node_management_service) {
  return {.view_service_ = UnownedService(view_service),
          .node_management_service_ = UnownedService(node_management_service),
          .history_service_ = UnownedService(history_service),
          .attribute_service_ = UnownedService(attribute_service),
          .method_service_ = UnownedService(method_service),
          .monitored_item_service_ = UnownedService(monitored_item_service)};
}

class ServerRuntimeTest : public testing::Test,
                          public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* typed = std::get_if<Response>(&body))
      return *typed;
    ADD_FAILURE() << "unexpected response type";
    return {};
  }

  std::pair<opcua::scada::NodeId, opcua::scada::NodeId> CreateAndActivate(
      ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    EXPECT_EQ(created.status.code(), opcua::scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = opcua::scada::LocalizedText{u"operator"},
                        .password = opcua::scada::LocalizedText{u"secret"},
                    });
    EXPECT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
    EXPECT_FALSE(activated.resumed);
    return {created.session_id, created.authentication_token};
  }

  void Detach(ConnectionState& connection) { runtime_.Detach(connection); }

  opcua::scada::StatusCode ReadStatus(ConnectionState& connection,
                               ReadRequest request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* response = std::get_if<ReadResponse>(&body))
      return response->status.code();
    if (const auto* fault = std::get_if<ServiceFault>(&body))
      return fault->status.code();
    return opcua::scada::StatusCode::Bad;
  }

  opcua::scada::StatusCode HistoryReadRawStatus(ConnectionState& connection,
                                         HistoryReadRawRequest request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* response = std::get_if<HistoryReadRawResponse>(&body))
      return response->result.status.code();
    if (const auto* fault = std::get_if<ServiceFault>(&body))
      return fault->status.code();
    return opcua::scada::StatusCode::Bad;
  }

  opcua::scada::StatusCode HistoryReadEventsStatus(ConnectionState& connection,
                                            HistoryReadEventsRequest request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* response = std::get_if<HistoryReadEventsResponse>(&body))
      return response->result.status.code();
    if (const auto* fault = std::get_if<ServiceFault>(&body))
      return fault->status.code();
    return opcua::scada::StatusCode::Bad;
  }

  bool capture_delayed_tasks_ = false;
  std::vector<std::pair<opcua::base::TimeDelta, std::function<void()>>> delayed_tasks_;

  ServerRuntime runtime_{ServerRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = attribute_service_,
      .view_service = view_service_,
      .history_service = history_service_,
      .method_service = method_service_,
      .node_management_service = node_management_service_,
      .now = [this] { return now_; },
      .post_delayed_task =
          [this](opcua::base::TimeDelta d, std::function<void()> fn) {
            if (capture_delayed_tasks_) {
              delayed_tasks_.emplace_back(d, std::move(fn));
              return;
            }
            executor_.PostDelayedTask(
                std::chrono::milliseconds{d.InMilliseconds()}, std::move(fn));
          },
  }};
};

TEST_F(ServerRuntimeTest, RoutesReadRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesReadRequestsThroughActivatedSessionUser(*this);
}

TEST_F(ServerRuntimeTest,
       ContextRoutesReadThroughNormalizedDataServices) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(907),
                  .attribute_id = opcua::scada::AttributeId::Value}}};
  EXPECT_CALL(attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](opcua::scada::ServiceContext context,
              std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs)
              -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_THAT(*inputs, testing::ElementsAre(request.inputs[0]));
            co_return std::vector{opcua::scada::DataValue{
                opcua::scada::Variant{907.0}, {}, now_, now_}};
          }));

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, opcua::scada::Variant{907.0});
}

TEST_F(ServerRuntimeTest, RoutesWriteRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesWriteRequestsThroughActivatedSessionUser(*this);
}

TEST_F(ServerRuntimeTest, RoutesCallRequestsThroughActivatedSessionUser) {
  test::ExpectRoutesCallRequestsThroughActivatedSessionUser(*this);
}

TEST_F(ServerRuntimeTest, PreservesLiveSubscriptionStateAcrossDetachAndResume) {
  test::ExpectPreservesLiveSubscriptionStateAcrossDetachAndResume(*this);
}

TEST_F(ServerRuntimeTest, TransfersSubscriptionsAcrossSessions) {
  test::ExpectTransfersSubscriptionsAcrossSessions(*this);
}

TEST_F(ServerRuntimeTest, CloseSessionClearsAttachedState) {
  test::ExpectCloseSessionClearsAttachedState(*this);
}

TEST_F(ServerRuntimeTest, RejectsHistoryReadRawWithoutActivatedSession) {
  test::ExpectRejectsHistoryReadRawWithoutActivatedSession(*this);
}

TEST_F(ServerRuntimeTest,
       HistoryReadRawPreservesPayloadThroughActivatedSession) {
  test::ExpectHistoryReadRawPreservesPayloadThroughActivatedSession(*this);
}

TEST_F(ServerRuntimeTest, RejectsHistoryReadEventsWithoutActivatedSession) {
  test::ExpectRejectsHistoryReadEventsWithoutActivatedSession(*this);
}

TEST_F(ServerRuntimeTest,
       HistoryReadEventsPreservesPayloadThroughActivatedSession) {
  test::ExpectHistoryReadEventsPreservesPayloadThroughActivatedSession(*this);
}

TEST_F(ServerRuntimeTest,
       BrowseAndBrowseNextUseSessionScopedContinuationPoints) {
  test::ExpectBrowseAndBrowseNextUseSessionScopedContinuationPoints(*this);
}

TEST_F(ServerRuntimeTest, NodeManagementMutationsPreserveBatchResults) {
  test::ExpectNodeManagementMutationsPreserveBatchResults(*this);
}

TEST_F(ServerRuntimeTest, PublishReturnsKeepAliveWhenNoNotificationsAreQueued) {
  test::ExpectPublishReturnsKeepAliveWhenNoNotifications(*this);
}

TEST_F(ServerRuntimeTest, RepublishReplaysNotificationUntilAcknowledged) {
  test::ExpectRepublishReplaysNotificationUntilAcknowledged(*this);
}

TEST_F(ServerRuntimeTest, PublishRequestWaitsForKeepAliveDeadline) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const auto created_subscription = HandleResponse<CreateSubscriptionResponse>(
      connection,
      CreateSubscriptionRequest{.parameters = {.publishing_interval_ms = 100,
                                               .lifetime_count = 60,
                                               .max_keep_alive_count = 3,
                                               .publishing_enabled = true}});
  EXPECT_EQ(created_subscription.status.code(), opcua::scada::StatusCode::Good);

  auto publish_result = opcua::StartAwaitable(
      executor_, runtime_.Handle(connection, RequestBody{PublishRequest{}}));

  const auto drain_ready = [&] {
    for (size_t i = 0; i < 8; ++i)
      executor_.Poll();
  };

  drain_ready();
  EXPECT_FALSE(publish_result->done);

  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(300);
  executor_.Advance(300ms);
  drain_ready();
  ASSERT_TRUE(publish_result->done);

  const auto publish_message = opcua::WaitResult(executor_, publish_result);
  const auto* publish = std::get_if<PublishResponse>(&publish_message);
  ASSERT_NE(publish, nullptr);
  EXPECT_EQ(publish->status.code(), opcua::scada::StatusCode::Good);
  EXPECT_TRUE(publish->notification_message.notification_data.empty());
}

TEST_F(ServerRuntimeTest, PublishDelayUsesInjectedSchedulerCallback) {
  capture_delayed_tasks_ = true;

  ConnectionState connection;
  CreateAndActivate(connection);

  const auto created_subscription = HandleResponse<CreateSubscriptionResponse>(
      connection,
      CreateSubscriptionRequest{.parameters = {.publishing_interval_ms = 100,
                                               .lifetime_count = 60,
                                               .max_keep_alive_count = 3,
                                               .publishing_enabled = true}});
  EXPECT_EQ(created_subscription.status.code(), opcua::scada::StatusCode::Good);

  auto publish_result = opcua::StartAwaitable(
      executor_, runtime_.Handle(connection, RequestBody{PublishRequest{}}));

  for (size_t i = 0; i < 8; ++i)
    executor_.Poll();

  ASSERT_EQ(delayed_tasks_.size(), 1u);
  EXPECT_EQ(delayed_tasks_.front().first,
            opcua::base::TimeDelta::FromMilliseconds(100));
  EXPECT_FALSE(publish_result->done);

  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(300);
  auto delayed = std::move(delayed_tasks_.front().second);
  delayed_tasks_.clear();
  delayed();
  for (size_t i = 0; i < 8; ++i)
    executor_.Poll();

  ASSERT_TRUE(publish_result->done);
  const auto publish_message = opcua::WaitResult(executor_, publish_result);
  const auto* publish = std::get_if<PublishResponse>(&publish_message);
  ASSERT_NE(publish, nullptr);
  EXPECT_EQ(publish->status.code(), opcua::scada::StatusCode::Good);
}

class CoroutineServerRuntimeTest : public testing::Test,
                                   public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* typed = std::get_if<Response>(&body))
      return *typed;
    ADD_FAILURE() << "unexpected response type";
    return {};
  }

  void CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    ASSERT_EQ(created.status.code(), opcua::scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = opcua::scada::LocalizedText{u"operator"},
                        .password = opcua::scada::LocalizedText{u"secret"},
                    });
    ASSERT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
  }

  test::TestCoroutineServices coroutine_services_;
  ServerRuntime runtime_{ServerRuntimeContext{
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

TEST_F(CoroutineServerRuntimeTest,
       RoutesReadThroughCoroutineServicesWithoutCallbackAdapters) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(901),
                  .attribute_id = opcua::scada::AttributeId::Value}}};

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, coroutine_services_.read_value);
  EXPECT_EQ(coroutine_services_.read_count, 1);
  EXPECT_EQ(coroutine_services_.last_read_context.user_id(), expected_user_id_);
  EXPECT_THAT(coroutine_services_.last_read_inputs,
              testing::ElementsAre(request.inputs[0]));
}

class DataServicesServerRuntimeTest
    : public testing::Test,
      public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* typed = std::get_if<Response>(&body))
      return *typed;
    ADD_FAILURE() << "unexpected response type";
    return {};
  }

  void CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    ASSERT_EQ(created.status.code(), opcua::scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = opcua::scada::LocalizedText{u"operator"},
                        .password = opcua::scada::LocalizedText{u"secret"},
                    });
    ASSERT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
  }

  std::shared_ptr<test::TestCoroutineServices> coroutine_services_ =
      std::make_shared<test::TestCoroutineServices>();
  ServerRuntime runtime_{DataServicesServerRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .data_services =
          MakeRuntimeDataServices(coroutine_services_, monitored_item_service_),
      .now = [this] { return now_; },
  }};
};

TEST_F(DataServicesServerRuntimeTest,
       RoutesReadThroughAggregateCoroutineSlotsWithoutCallbackAdapters) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(903),
                  .attribute_id = opcua::scada::AttributeId::Value}}};

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, coroutine_services_->read_value);
  EXPECT_EQ(coroutine_services_->read_count, 1);
  EXPECT_EQ(coroutine_services_->last_read_context.user_id(),
            expected_user_id_);
  EXPECT_THAT(coroutine_services_->last_read_inputs,
              testing::ElementsAre(request.inputs[0]));
}

TEST_F(DataServicesServerRuntimeTest, GetEndpointsRebasesMatchingSchemeOnly) {
  std::vector<EndpointDescription> endpoints = {
      {.endpoint_url = "opc.tcp://0.0.0.0:4840",
       .transport_profile_uri =
           "http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary"},
      {.endpoint_url = "opc.wss://0.0.0.0:4843",
       .transport_profile_uri =
           "http://opcfoundation.org/UA-Profile/Transport/wss-uajson"},
  };
  ServerRuntime runtime{DataServicesServerRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .data_services = MakeRuntimeDataServices(coroutine_services_,
                                               monitored_item_service_),
      .endpoints = endpoints,
      .now = [this] { return now_; },
  }};

  ConnectionState connection;
  const auto body = opcua::WaitAwaitable(
      executor_,
      runtime.Handle(connection, RequestBody{GetEndpointsRequest{
                                     .endpoint_url = "opc.tcp://gateway:4840"}}));
  const auto* response = std::get_if<GetEndpointsResponse>(&body);
  ASSERT_TRUE(response);
  ASSERT_EQ(response->endpoints.size(), 2u);

  std::string tcp_url;
  std::string wss_url;
  for (const auto& endpoint : response->endpoints) {
    if (endpoint.endpoint_url.starts_with("opc.tcp"))
      tcp_url = endpoint.endpoint_url;
    else
      wss_url = endpoint.endpoint_url;
  }
  // The TCP endpoint is rebased onto the client-reachable host; the WS endpoint
  // (different scheme) keeps its configured URL.
  EXPECT_EQ(tcp_url, "opc.tcp://gateway:4840");
  EXPECT_EQ(wss_url, "opc.wss://0.0.0.0:4843");
}

TEST_F(DataServicesServerRuntimeTest, RejectsRequestsExceedingOperationLimits) {
  ServerRuntime limited{DataServicesServerRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .data_services = MakeRuntimeDataServices(coroutine_services_,
                                               monitored_item_service_),
      .operation_limits =
          OperationLimits{.max_nodes_per_read = 1, .max_nodes_per_method_call = 1},
      .now = [this] { return now_; },
  }};

  ConnectionState connection;
  const auto created_body = opcua::WaitAwaitable(
      executor_, limited.Handle(connection, RequestBody{CreateSessionRequest{}}));
  const auto* created = std::get_if<CreateSessionResponse>(&created_body);
  ASSERT_TRUE(created);
  const auto activated_body = opcua::WaitAwaitable(
      executor_,
      limited.Handle(connection,
                     RequestBody{ActivateSessionRequest{
                         .session_id = created->session_id,
                         .authentication_token = created->authentication_token,
                         .user_name = opcua::scada::LocalizedText{u"operator"},
                         .password = opcua::scada::LocalizedText{u"secret"},
                     }}));
  ASSERT_TRUE(std::get_if<ActivateSessionResponse>(&activated_body));

  // A Read with two nodes exceeds max_nodes_per_read = 1.
  const ReadRequest read_request{
      .inputs = {{.node_id = test::NumericNode(903),
                  .attribute_id = opcua::scada::AttributeId::Value},
                 {.node_id = test::NumericNode(904),
                  .attribute_id = opcua::scada::AttributeId::Value}}};
  const auto read_body = opcua::WaitAwaitable(
      executor_, limited.Handle(connection, RequestBody{read_request}));
  const auto* read_response = std::get_if<ReadResponse>(&read_body);
  ASSERT_TRUE(read_response);
  EXPECT_EQ(read_response->status.code(),
            opcua::scada::StatusCode::Bad_TooManyOperations);

  // A Call with two methods exceeds max_nodes_per_method_call = 1.
  const CallRequest call_request{
      .methods = {MethodCallRequest{}, MethodCallRequest{}}};
  const auto call_body = opcua::WaitAwaitable(
      executor_, limited.Handle(connection, RequestBody{call_request}));
  const auto* call_response = std::get_if<CallResponse>(&call_body);
  ASSERT_TRUE(call_response);
  EXPECT_EQ(call_response->status.code(),
            opcua::scada::StatusCode::Bad_TooManyOperations);

  // An empty operation array is Bad_NothingToDo (OPC UA Part 4 §5.10).
  const auto empty_read_body = opcua::WaitAwaitable(
      executor_, limited.Handle(connection, RequestBody{ReadRequest{}}));
  const auto* empty_read = std::get_if<ReadResponse>(&empty_read_body);
  ASSERT_TRUE(empty_read);
  EXPECT_EQ(empty_read->status.code(), opcua::scada::StatusCode::Bad_NothingToDo);

  const auto empty_call_body = opcua::WaitAwaitable(
      executor_, limited.Handle(connection, RequestBody{CallRequest{}}));
  const auto* empty_call = std::get_if<CallResponse>(&empty_call_body);
  ASSERT_TRUE(empty_call);
  EXPECT_EQ(empty_call->status.code(), opcua::scada::StatusCode::Bad_NothingToDo);
}

class DataServicesCallbackServerRuntimeTest
    : public testing::Test,
      public test::ServerRuntimeContractTestBase {
 public:
  using ConnectionState = opcua::ConnectionState;

  template <typename Response, typename Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    const auto body = opcua::WaitAwaitable(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));
    if (const auto* typed = std::get_if<Response>(&body))
      return *typed;
    ADD_FAILURE() << "unexpected response type";
    return {};
  }

  void CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    ASSERT_EQ(created.status.code(), opcua::scada::StatusCode::Good);

    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = opcua::scada::LocalizedText{u"operator"},
                        .password = opcua::scada::LocalizedText{u"secret"},
                    });
    ASSERT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
  }

  ServerRuntime runtime_{DataServicesServerRuntimeContext{
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

TEST_F(DataServicesCallbackServerRuntimeTest,
       RoutesReadThroughAggregateUnownedSlots) {
  ConnectionState connection;
  CreateAndActivate(connection);

  const ReadRequest request{
      .inputs = {{.node_id = test::NumericNode(905),
                  .attribute_id = opcua::scada::AttributeId::Value}}};
  EXPECT_CALL(attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](opcua::scada::ServiceContext context,
              std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs)
              -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_THAT(*inputs, testing::ElementsAre(request.inputs[0]));
            co_return std::vector{opcua::scada::DataValue{
                opcua::scada::Variant{905.0}, {}, now_, now_}};
          }));

  const auto response = HandleResponse<ReadResponse>(connection, request);

  EXPECT_EQ(response.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, opcua::scada::Variant{905.0});
}

}  // namespace
}  // namespace opcua
