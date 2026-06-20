#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/message.h"
#include "opcua/server_session_manager.h"
#include "opcua/scada/attribute_service_mock.h"
#include "opcua/scada/authentication_adapters.h"
#include "opcua/scada/coroutine_services.h"
#include "opcua/scada/history_service_mock.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/method_service_mock.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/node_management_service_mock.h"
#include "opcua/scada/test/test_monitored_item.h"
#include "opcua/scada/view_service_mock.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace opcua::test {

inline scada::NodeId NumericNode(scada::NumericId id,
                                 scada::NamespaceIndex ns = 2) {
  return {id, ns};
}

inline base::Time ParseTime(std::string_view value) {
  base::Time result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

class TestMonitoredItemService : public scada::MonitoredItemService {
 public:
  std::shared_ptr<scada::MonitoredItem> CreateMonitoredItem(
      const scada::ReadValueId& value_id,
      const scada::MonitoringParameters& params) {
    created_value_ids.push_back(value_id);
    created_params.push_back(params);
    auto item = std::make_shared<scada::TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  scada::StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
  CreateSubscription(scada::ServiceContext /*context*/,
                     scada::MonitoredItemSubscriptionOptions options) override {
    return scada::MakeItemFactorySubscription(
        [this](const scada::ReadValueId& value_id,
               const scada::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  std::vector<scada::ReadValueId> created_value_ids;
  std::vector<scada::MonitoringParameters> created_params;
  std::vector<std::shared_ptr<scada::TestMonitoredItem>> items;
};

class TestCoroutineServices final : public scada::AttributeService,
                                    public scada::ViewService,
                                    public scada::HistoryService,
                                    public scada::MethodService,
                                    public scada::NodeManagementService {
 public:
  Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> Read(
      scada::ServiceContext context,
      std::shared_ptr<const std::vector<scada::ReadValueId>> inputs) override {
    ++read_count;
    last_read_context = std::move(context);
    last_read_inputs = *inputs;
    co_return std::vector<scada::DataValue>{
        scada::DataValue{read_value, {}, timestamp, timestamp}};
  }

  Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> Write(
      scada::ServiceContext context,
      std::shared_ptr<const std::vector<scada::WriteValue>> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>> Browse(
      scada::ServiceContext context,
      std::vector<scada::BrowseDescription> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::StatusOr<std::vector<scada::BrowsePathResult>>>
  TranslateBrowsePaths(std::vector<scada::BrowsePath> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::HistoryReadRawResult> HistoryReadRaw(
      scada::HistoryReadRawDetails details) override {
    co_return scada::HistoryReadRawResult{.status = scada::StatusCode::Bad};
  }

  Awaitable<scada::HistoryReadEventsResult> HistoryReadEvents(
      scada::NodeId node_id,
      base::Time from,
      base::Time to,
      scada::EventFilter filter) override {
    co_return scada::HistoryReadEventsResult{.status = scada::StatusCode::Bad};
  }

  Awaitable<scada::Status> Call(scada::NodeId node_id,
                                scada::NodeId method_id,
                                std::vector<scada::Variant> arguments,
                                scada::NodeId user_id) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::StatusOr<std::vector<scada::AddNodesResult>>> AddNodes(
      std::vector<scada::AddNodesItem> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> DeleteNodes(
      std::vector<scada::DeleteNodesItem> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> AddReferences(
      std::vector<scada::AddReferencesItem> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> DeleteReferences(
      std::vector<scada::DeleteReferencesItem> inputs) override {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  base::Time timestamp = ParseTime("2026-04-22 09:01:00");
  scada::Variant read_value = 123.0;
  int read_count = 0;
  scada::ServiceContext last_read_context;
  std::vector<scada::ReadValueId> last_read_inputs;
};

class ServerRuntimeContractTestBase {
 public:
  base::Time now_ = ParseTime("2026-04-22 09:00:00");
  const scada::NodeId expected_user_id_ = NumericNode(700, 5);
  TestExecutor executor_;
  const AnyExecutor any_executor_ = executor_;
  testing::StrictMock<scada::MockAttributeService> attribute_service_;
  testing::StrictMock<scada::MockViewService> view_service_;
  testing::StrictMock<scada::MockHistoryService> history_service_;
  testing::StrictMock<scada::MockMethodService> method_service_;
  testing::StrictMock<scada::MockNodeManagementService>
      node_management_service_;
  TestMonitoredItemService monitored_item_service_;
  ServerSessionManager session_manager_{{
      .authenticator = scada::MakeCoroutineAuthenticator(
          [this](scada::LocalizedText user_name, scada::LocalizedText password)
              -> Awaitable<scada::StatusOr<scada::AuthenticationResult>> {
            EXPECT_EQ(user_name, scada::LocalizedText{u"operator"});
            EXPECT_EQ(password, scada::LocalizedText{u"secret"});
            co_return scada::AuthenticationResult{.user_id = expected_user_id_,
                                                  .multi_sessions = true};
          }),
      .now = [this] { return now_; },
  }};
};

template <typename Fixture>
void ExpectRoutesReadRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  ReadRequest request{
      .inputs = {{.node_id = NumericNode(1),
                  .attribute_id = scada::AttributeId::DisplayName}}};
  EXPECT_CALL(fixture.attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::ServiceContext context,
              std::shared_ptr<const std::vector<scada::ReadValueId>> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
            EXPECT_THAT(*inputs, testing::ElementsAre(request.inputs[0]));
            co_return std::vector{scada::DataValue{
                scada::LocalizedText{u"Pump"}, {}, fixture.now_, fixture.now_}};
          }));

  const auto response =
      fixture.template HandleResponse<ReadResponse>(connection, request);
  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value,
            scada::Variant{scada::LocalizedText{u"Pump"}});
}

template <typename Fixture>
void ExpectRoutesWriteRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  WriteRequest request;
  request.inputs.push_back(scada::WriteValue{
      .node_id = NumericNode(2),
      .attribute_id = scada::AttributeId::Value,
      .value = scada::Variant{17.5},
  });
  EXPECT_CALL(fixture.attribute_service_, Write(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::ServiceContext context,
              std::shared_ptr<const std::vector<scada::WriteValue>> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> {
            EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
            EXPECT_EQ(inputs->size(), 1u);
            if (inputs->size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            EXPECT_EQ((*inputs)[0].node_id, request.inputs[0].node_id);
            EXPECT_EQ((*inputs)[0].attribute_id,
                      request.inputs[0].attribute_id);
            EXPECT_EQ((*inputs)[0].value, request.inputs[0].value);
            co_return std::vector<scada::StatusCode>{
                scada::StatusCode::Good_Manual};
          }));

  const auto response =
      fixture.template HandleResponse<WriteResponse>(connection, request);
  EXPECT_EQ(response.status.code(), scada::StatusCode::Good);
  EXPECT_THAT(response.results,
              testing::ElementsAre(static_cast<scada::StatusCode>(
                  scada::StatusCode::Good_Manual)));
}

template <typename Fixture>
void ExpectRoutesCallRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  CallRequest request{
      .methods = {{.object_id = NumericNode(301),
                   .method_id = NumericNode(302),
                   .arguments = {scada::Variant{7.0},
                                 scada::Variant{std::string{"mode"}}}}}};
  EXPECT_CALL(fixture.method_service_,
              Call(request.methods[0].object_id, request.methods[0].method_id,
                   request.methods[0].arguments, fixture.expected_user_id_))
      .WillOnce(testing::Invoke([](scada::NodeId, scada::NodeId,
                                   std::vector<scada::Variant>, scada::NodeId) {
        return scada::MakeMethodCallResult(scada::StatusCode::Good);
      }));

  const auto response =
      fixture.template HandleResponse<CallResponse>(connection, request);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].status.code(), scada::StatusCode::Good);
  EXPECT_TRUE(response.results[0].input_argument_results.empty());
  EXPECT_TRUE(response.results[0].output_arguments.empty());
}

template <typename Fixture>
void ExpectHistoryReadRawPreservesPayloadThroughActivatedSession(
    Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto from = fixture.now_ - base::TimeDelta::FromMinutes(15);
  const auto to = fixture.now_;
  HistoryReadRawRequest request{
      .details = {
          .node_id = NumericNode(401), .from = from, .to = to, .max_count = 3}};
  EXPECT_CALL(fixture.history_service_, HistoryReadRaw(testing::_))
      .WillOnce(testing::Invoke([&](scada::HistoryReadRawDetails details)
                                    -> Awaitable<scada::HistoryReadRawResult> {
        EXPECT_TRUE(details.node_id == request.details.node_id);
        EXPECT_EQ(details.from, from);
        EXPECT_EQ(details.to, to);
        EXPECT_EQ(details.max_count, 3u);
        co_return scada::HistoryReadRawResult{
            .status = scada::StatusCode::Good,
            .values = {scada::DataValue{
                scada::Variant{12.5}, {}, fixture.now_, fixture.now_}},
            .continuation_point = {1, 2, 3},
        };
      }));

  const auto response = fixture.template HandleResponse<HistoryReadRawResponse>(
      connection, request);
  EXPECT_EQ(response.result.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.result.values.size(), 1u);
  EXPECT_EQ(response.result.values[0].value, scada::Variant{12.5});
  EXPECT_EQ(response.result.continuation_point, (scada::ByteString{1, 2, 3}));
}

template <typename Fixture>
void ExpectHistoryReadEventsPreservesPayloadThroughActivatedSession(
    Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto from = fixture.now_ - base::TimeDelta::FromHours(4);
  const auto to = fixture.now_;
  HistoryReadEventsRequest request{
      .details = {
          .node_id = NumericNode(402), .from = from, .to = to, .filter = {}}};
  EXPECT_CALL(fixture.history_service_,
              HistoryReadEvents(testing::_, testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::NodeId node_id, base::Time actual_from,
              base::Time actual_to,
              scada::EventFilter) -> Awaitable<scada::HistoryReadEventsResult> {
            EXPECT_EQ(node_id, request.details.node_id);
            EXPECT_EQ(actual_from, from);
            EXPECT_EQ(actual_to, to);
            scada::Event event;
            event.event_id = 99;
            event.time = fixture.now_;
            event.receive_time = fixture.now_;
            event.node_id = NumericNode(403);
            event.message = scada::LocalizedText{u"alarm"};
            co_return scada::HistoryReadEventsResult{
                .status = scada::StatusCode::Good,
                .events = {std::move(event)},
            };
          }));

  const auto response =
      fixture.template HandleResponse<HistoryReadEventsResponse>(connection,
                                                                 request);
  EXPECT_EQ(response.result.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(response.result.events.size(), 1u);
  EXPECT_EQ(response.result.events[0].event_id, 99u);
  EXPECT_TRUE(response.result.events[0].node_id == NumericNode(403));
}

template <typename Fixture>
void ExpectNodeManagementMutationsPreserveBatchResults(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  AddNodesRequest add_nodes{
      .items = {{.requested_id = NumericNode(501),
                 .parent_id = NumericNode(502),
                 .type_definition_id = NumericNode(503)}}};
  DeleteNodesRequest delete_nodes{
      .items = {
          {.node_id = NumericNode(504), .delete_target_references = true}}};
  AddReferencesRequest add_references{
      .items = {{.source_node_id = NumericNode(505),
                 .reference_type_id = NumericNode(506),
                 .target_node_id = scada::ExpandedNodeId{NumericNode(507)}}}};
  DeleteReferencesRequest delete_references{
      .items = {{.source_node_id = NumericNode(508),
                 .reference_type_id = NumericNode(509),
                 .target_node_id = scada::ExpandedNodeId{NumericNode(510)}}}};

  EXPECT_CALL(fixture.node_management_service_, AddNodes(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<scada::AddNodesItem> items)
              -> Awaitable<
                  scada::StatusOr<std::vector<scada::AddNodesResult>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].requested_id, add_nodes.items[0].requested_id);
            EXPECT_EQ(items[0].parent_id, add_nodes.items[0].parent_id);
            EXPECT_EQ(items[0].type_definition_id,
                      add_nodes.items[0].type_definition_id);
            co_return std::vector{scada::AddNodesResult{
                .status_code = scada::StatusCode::Good,
                .added_node_id = NumericNode(511),
            }};
          }));
  EXPECT_CALL(fixture.node_management_service_, DeleteNodes(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<scada::DeleteNodesItem> items)
              -> Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].node_id, delete_nodes.items[0].node_id);
            EXPECT_TRUE(items[0].delete_target_references);
            co_return std::vector{scada::StatusCode::Good,
                                  scada::StatusCode::Bad_WrongNodeId};
          }));
  EXPECT_CALL(fixture.node_management_service_, AddReferences(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<scada::AddReferencesItem> items)
              -> Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].source_node_id,
                      add_references.items[0].source_node_id);
            EXPECT_EQ(items[0].reference_type_id,
                      add_references.items[0].reference_type_id);
            EXPECT_EQ(items[0].target_node_id,
                      add_references.items[0].target_node_id);
            co_return std::vector{scada::StatusCode::Good,
                                  scada::StatusCode::Bad_WrongTargetId};
          }));
  EXPECT_CALL(fixture.node_management_service_, DeleteReferences(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<scada::DeleteReferencesItem> items)
              -> Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].source_node_id,
                      delete_references.items[0].source_node_id);
            EXPECT_EQ(items[0].reference_type_id,
                      delete_references.items[0].reference_type_id);
            EXPECT_EQ(items[0].target_node_id,
                      delete_references.items[0].target_node_id);
            co_return scada::Status{scada::StatusCode::Bad_Disconnected};
          }));

  const auto add_nodes_response =
      fixture.template HandleResponse<AddNodesResponse>(connection, add_nodes);
  EXPECT_EQ(add_nodes_response.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(add_nodes_response.results.size(), 1u);
  EXPECT_EQ(add_nodes_response.results[0].status_code, scada::StatusCode::Good);
  EXPECT_EQ(add_nodes_response.results[0].added_node_id, NumericNode(511));

  const auto delete_nodes_response =
      fixture.template HandleResponse<DeleteNodesResponse>(connection,
                                                           delete_nodes);
  EXPECT_EQ(delete_nodes_response.status.code(), scada::StatusCode::Good);
  EXPECT_THAT(delete_nodes_response.results,
              testing::ElementsAre(scada::StatusCode::Good,
                                   scada::StatusCode::Bad_WrongNodeId));

  const auto add_references_response =
      fixture.template HandleResponse<AddReferencesResponse>(connection,
                                                             add_references);
  EXPECT_EQ(add_references_response.status.code(), scada::StatusCode::Good);
  EXPECT_THAT(add_references_response.results,
              testing::ElementsAre(scada::StatusCode::Good,
                                   scada::StatusCode::Bad_WrongTargetId));

  const auto delete_references_response =
      fixture.template HandleResponse<DeleteReferencesResponse>(
          connection, delete_references);
  EXPECT_EQ(delete_references_response.status.code(),
            scada::StatusCode::Bad_Disconnected);
  EXPECT_TRUE(delete_references_response.results.empty());
}

template <typename Fixture>
void ExpectPreservesLiveSubscriptionStateAcrossDetachAndResume(
    Fixture& fixture) {
  typename Fixture::ConnectionState first_connection;
  const auto [session_id, authentication_token] =
      fixture.CreateAndActivate(first_connection);

  const auto subscription =
      fixture.template HandleResponse<CreateSubscriptionResponse>(
          first_connection, CreateSubscriptionRequest{
                                .parameters = {.publishing_interval_ms = 100,
                                               .lifetime_count = 60,
                                               .max_keep_alive_count = 3,
                                               .publishing_enabled = true}});
  ASSERT_EQ(subscription.status.code(), scada::StatusCode::Good);

  const auto create_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          first_connection,
          CreateMonitoredItemsRequest{
              .subscription_id = subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(11),
                                       .attribute_id =
                                           scada::AttributeId::Value},
                   .requested_parameters = {.client_handle = 44,
                                            .sampling_interval_ms = 0,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  ASSERT_EQ(create_items.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(fixture.monitored_item_service_.items.size(), 1u);

  fixture.monitored_item_service_.items[0]->NotifyDataChange(
      scada::DataValue{scada::Variant{12.5}, {}, fixture.now_, fixture.now_});
  fixture.Detach(first_connection);
  EXPECT_FALSE(first_connection.authentication_token.has_value());

  typename Fixture::ConnectionState second_connection;
  const auto resumed = fixture.template HandleResponse<ActivateSessionResponse>(
      second_connection, ActivateSessionRequest{
                             .session_id = session_id,
                             .authentication_token = authentication_token,
                         });
  EXPECT_EQ(resumed.status.code(), scada::StatusCode::Good);
  EXPECT_TRUE(resumed.resumed);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(100);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      second_connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), scada::StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, subscription.subscription_id);
  const auto* data = std::get_if<DataChangeNotification>(
      &publish.notification_message.notification_data[0]);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->monitored_items[0].value.value.template get<double>(), 12.5);
}

template <typename Fixture>
void ExpectTransfersSubscriptionsAcrossSessions(Fixture& fixture) {
  typename Fixture::ConnectionState source_connection;
  fixture.CreateAndActivate(source_connection);

  const auto created_subscription =
      fixture.template HandleResponse<CreateSubscriptionResponse>(
          source_connection, CreateSubscriptionRequest{
                                 .parameters = {.publishing_interval_ms = 100,
                                                .lifetime_count = 60,
                                                .max_keep_alive_count = 3,
                                                .publishing_enabled = true}});
  ASSERT_EQ(created_subscription.status.code(), scada::StatusCode::Good);

  const auto created_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          source_connection,
          CreateMonitoredItemsRequest{
              .subscription_id = created_subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(21),
                                       .attribute_id =
                                           scada::AttributeId::Value},
                   .requested_parameters = {.client_handle = 55,
                                            .sampling_interval_ms = 0,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  ASSERT_EQ(created_items.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(fixture.monitored_item_service_.items.size(), 1u);

  fixture.monitored_item_service_.items[0]->NotifyDataChange(
      scada::DataValue{scada::Variant{77.0}, {}, fixture.now_, fixture.now_});

  typename Fixture::ConnectionState target_connection;
  fixture.CreateAndActivate(target_connection);
  const auto transferred =
      fixture.template HandleResponse<TransferSubscriptionsResponse>(
          target_connection,
          TransferSubscriptionsRequest{
              .subscription_ids = {created_subscription.subscription_id},
              .send_initial_values = true});
  EXPECT_EQ(transferred.results,
            (std::vector<scada::StatusCode>{scada::StatusCode::Good}));

  const auto source_publish = fixture.template HandleResponse<PublishResponse>(
      source_connection, PublishRequest{});
  EXPECT_EQ(source_publish.status.code(), scada::StatusCode::Bad_NothingToDo);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(100);
  const auto target_publish = fixture.template HandleResponse<PublishResponse>(
      target_connection, PublishRequest{});
  EXPECT_EQ(target_publish.subscription_id,
            created_subscription.subscription_id);
  const auto* data = std::get_if<DataChangeNotification>(
      &target_publish.notification_message.notification_data[0]);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->monitored_items[0].value.value.template get<double>(), 77.0);
}

template <typename Fixture>
void ExpectCloseSessionClearsAttachedState(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  const auto [session_id, authentication_token] =
      fixture.CreateAndActivate(connection);

  const auto closed = fixture.template HandleResponse<CloseSessionResponse>(
      connection, CloseSessionRequest{
                      .session_id = session_id,
                      .authentication_token = authentication_token,
                  });
  EXPECT_EQ(closed.status.code(), scada::StatusCode::Good);
  EXPECT_FALSE(connection.authentication_token.has_value());

  const auto status = fixture.ReadStatus(
      connection,
      ReadRequest{.inputs = {{.node_id = NumericNode(31),
                              .attribute_id = scada::AttributeId::Value}}});
  EXPECT_EQ(status, scada::StatusCode::Bad_SessionIsLoggedOff);
}

template <typename Fixture>
void ExpectRejectsHistoryReadRawWithoutActivatedSession(Fixture& fixture) {
  typename Fixture::ConnectionState connection;

  const auto status = fixture.HistoryReadRawStatus(
      connection,
      HistoryReadRawRequest{
          .details = {.node_id = NumericNode(41),
                      .from = fixture.now_ - base::TimeDelta::FromMinutes(10),
                      .to = fixture.now_,
                      .max_count = 5}});
  EXPECT_EQ(status, scada::StatusCode::Bad_SessionIsLoggedOff);
}

template <typename Fixture>
void ExpectRejectsHistoryReadEventsWithoutActivatedSession(Fixture& fixture) {
  typename Fixture::ConnectionState connection;

  const auto status = fixture.HistoryReadEventsStatus(
      connection,
      HistoryReadEventsRequest{
          .details = {.node_id = NumericNode(42),
                      .from = fixture.now_ - base::TimeDelta::FromMinutes(30),
                      .to = fixture.now_}});
  EXPECT_EQ(status, scada::StatusCode::Bad_SessionIsLoggedOff);
}

template <typename Fixture>
void ExpectBrowseAndBrowseNextUseSessionScopedContinuationPoints(
    Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  EXPECT_CALL(fixture.view_service_, Browse(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](scada::ServiceContext context,
              std::vector<scada::BrowseDescription> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>> {
            EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u) {
              co_return scada::Status{scada::StatusCode::Bad};
            }
            co_return std::vector<scada::BrowseResult>{scada::BrowseResult{
                .status_code = scada::StatusCode::Good,
                .references = {{.reference_type_id = NumericNode(901),
                                .forward = true,
                                .node_id = NumericNode(902)},
                               {.reference_type_id = NumericNode(903),
                                .forward = false,
                                .node_id = NumericNode(904)},
                               {.reference_type_id = NumericNode(905),
                                .forward = true,
                                .node_id = NumericNode(906)}}}};
          }));

  const auto browse = fixture.template HandleResponse<BrowseResponse>(
      connection,
      BrowseRequest{.requested_max_references_per_node = 2,
                    .inputs = {{.node_id = NumericNode(900),
                                .direction = scada::BrowseDirection::Both,
                                .reference_type_id = NumericNode(910),
                                .include_subtypes = true}}});
  ASSERT_EQ(browse.results.size(), 1u);
  ASSERT_EQ(browse.results[0].references.size(), 2u);
  ASSERT_FALSE(browse.results[0].continuation_point.empty());
  EXPECT_EQ(browse.results[0].references[0].node_id, NumericNode(902));
  EXPECT_EQ(browse.results[0].references[1].node_id, NumericNode(904));

  typename Fixture::ConnectionState other_connection;
  fixture.CreateAndActivate(other_connection);
  const auto wrong_session =
      fixture.template HandleResponse<BrowseNextResponse>(
          other_connection,
          BrowseNextRequest{
              .continuation_points = {browse.results[0].continuation_point}});
  ASSERT_EQ(wrong_session.results.size(), 1u);
  EXPECT_EQ(wrong_session.results[0].status_code,
            scada::StatusCode::Bad_WrongIndex);

  const auto browse_next = fixture.template HandleResponse<BrowseNextResponse>(
      connection, BrowseNextRequest{.continuation_points = {
                                        browse.results[0].continuation_point}});
  ASSERT_EQ(browse_next.results.size(), 1u);
  EXPECT_EQ(browse_next.results[0].status_code, scada::StatusCode::Good);
  ASSERT_EQ(browse_next.results[0].references.size(), 1u);
  EXPECT_EQ(browse_next.results[0].references[0].node_id, NumericNode(906));
  EXPECT_TRUE(browse_next.results[0].continuation_point.empty());

  const auto invalid = fixture.template HandleResponse<BrowseNextResponse>(
      connection, BrowseNextRequest{.continuation_points = {
                                        browse.results[0].continuation_point}});
  ASSERT_EQ(invalid.results.size(), 1u);
  EXPECT_EQ(invalid.results[0].status_code, scada::StatusCode::Bad_WrongIndex);
}

template <typename Fixture>
void ExpectPublishReturnsKeepAliveWhenNoNotifications(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto created_subscription =
      fixture.template HandleResponse<CreateSubscriptionResponse>(
          connection, CreateSubscriptionRequest{
                          .parameters = {.publishing_interval_ms = 100,
                                         .lifetime_count = 60,
                                         .max_keep_alive_count = 3,
                                         .publishing_enabled = true}});
  ASSERT_EQ(created_subscription.status.code(), scada::StatusCode::Good);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(300);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), scada::StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, created_subscription.subscription_id);
  EXPECT_TRUE(publish.notification_message.notification_data.empty());
  EXPECT_EQ(publish.notification_message.sequence_number, 1u);
  EXPECT_TRUE(publish.available_sequence_numbers.empty());
}

template <typename Fixture>
void ExpectRepublishReplaysNotificationUntilAcknowledged(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto created_subscription =
      fixture.template HandleResponse<CreateSubscriptionResponse>(
          connection, CreateSubscriptionRequest{
                          .parameters = {.publishing_interval_ms = 100,
                                         .lifetime_count = 60,
                                         .max_keep_alive_count = 3,
                                         .publishing_enabled = true}});
  ASSERT_EQ(created_subscription.status.code(), scada::StatusCode::Good);

  const auto create_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          connection,
          CreateMonitoredItemsRequest{
              .subscription_id = created_subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(51),
                                       .attribute_id =
                                           scada::AttributeId::Value},
                   .requested_parameters = {.client_handle = 88,
                                            .sampling_interval_ms = 0,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  ASSERT_EQ(create_items.status.code(), scada::StatusCode::Good);
  ASSERT_EQ(fixture.monitored_item_service_.items.size(), 1u);

  fixture.monitored_item_service_.items[0]->NotifyDataChange(
      scada::DataValue{scada::Variant{42.5}, {}, fixture.now_, fixture.now_});
  // The notification flows through the subscription pump's async read loop
  // (which parks on an asio steady_timer), so spin the executor until the value
  // reaches the queue before publishing.
  for (int i = 0; i < 200; ++i) {
    Drain(fixture.executor_);
    std::this_thread::yield();
  }
  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(100);

  const auto publish = fixture.template HandleResponse<PublishResponse>(
      connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), scada::StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, created_subscription.subscription_id);
  EXPECT_EQ(publish.available_sequence_numbers,
            (std::vector<scada::UInt32>{1u}));
  ASSERT_EQ(publish.notification_message.notification_data.size(), 1u);
  const auto* published_data = std::get_if<DataChangeNotification>(
      &publish.notification_message.notification_data[0]);
  ASSERT_NE(published_data, nullptr);
  ASSERT_EQ(published_data->monitored_items.size(), 1u);
  EXPECT_EQ(published_data->monitored_items[0].client_handle, 88u);
  EXPECT_EQ(
      published_data->monitored_items[0].value.value.template get<double>(),
      42.5);

  const auto republish = fixture.template HandleResponse<RepublishResponse>(
      connection,
      RepublishRequest{.subscription_id = created_subscription.subscription_id,
                       .retransmit_sequence_number =
                           publish.notification_message.sequence_number});
  EXPECT_EQ(republish.status.code(), scada::StatusCode::Good);
  EXPECT_EQ(republish.notification_message, publish.notification_message);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(300);
  const auto ack_publish = fixture.template HandleResponse<PublishResponse>(
      connection,
      PublishRequest{
          .subscription_acknowledgements = {{
              .subscription_id = created_subscription.subscription_id,
              .sequence_number = publish.notification_message.sequence_number,
          }}});
  EXPECT_EQ(ack_publish.status.code(), scada::StatusCode::Good);
  EXPECT_EQ(ack_publish.results,
            (std::vector<scada::StatusCode>{scada::StatusCode::Good}));
  EXPECT_TRUE(ack_publish.available_sequence_numbers.empty());

  const auto after_ack = fixture.template HandleResponse<RepublishResponse>(
      connection,
      RepublishRequest{.subscription_id = created_subscription.subscription_id,
                       .retransmit_sequence_number =
                           publish.notification_message.sequence_number});
  EXPECT_EQ(after_ack.status.code(),
            scada::StatusCode::Bad_MessageNotAvailable);
}

}  // namespace opcua::test
