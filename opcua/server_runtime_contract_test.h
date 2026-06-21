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
#include "opcua/scada/node_management_service_mock.h"
#include "opcua/scada/test/test_monitored_item.h"
#include "opcua/scada/view_service_mock.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace opcua::test {

inline NodeId NumericNode(NumericId id,
                                 NamespaceIndex ns = 2) {
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
      const ReadValueId& value_id,
      const MonitoringParameters& params) {
    created_value_ids.push_back(value_id);
    created_params.push_back(params);
    auto item = std::make_shared<TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
  CreateSubscription(ServiceContext /*context*/,
                     scada::MonitoredItemSubscriptionOptions options) override {
    return scada::MakeItemFactorySubscription(
        [this](const ReadValueId& value_id,
               const MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  std::vector<ReadValueId> created_value_ids;
  std::vector<MonitoringParameters> created_params;
  std::vector<std::shared_ptr<TestMonitoredItem>> items;
};

class TestCoroutineServices final : public AttributeService,
                                    public ViewService,
                                    public HistoryService,
                                    public MethodService,
                                    public NodeManagementService {
 public:
  Awaitable<StatusOr<std::vector<DataValue>>> Read(
      ServiceContext context,
      std::shared_ptr<const std::vector<ReadValueId>> inputs) override {
    ++read_count;
    last_read_context = std::move(context);
    last_read_inputs = *inputs;
    co_return std::vector<DataValue>{
        DataValue{read_value, {}, timestamp, timestamp}};
  }

  Awaitable<StatusOr<std::vector<StatusCode>>> Write(
      ServiceContext context,
      std::shared_ptr<const std::vector<WriteValue>> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<StatusOr<std::vector<BrowseResult>>> Browse(
      ServiceContext context,
      std::vector<BrowseDescription> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<StatusOr<std::vector<BrowsePathResult>>>
  TranslateBrowsePaths(std::vector<BrowsePath> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<HistoryReadRawResult> HistoryReadRaw(
      HistoryReadRawDetails details) override {
    co_return HistoryReadRawResult{.status = StatusCode::Bad};
  }

  Awaitable<HistoryReadEventsResult> HistoryReadEvents(
      NodeId node_id,
      base::Time from,
      base::Time to,
      EventFilter filter) override {
    co_return HistoryReadEventsResult{.status = StatusCode::Bad};
  }

  Awaitable<Status> Call(NodeId node_id,
                                NodeId method_id,
                                std::vector<Variant> arguments,
                                NodeId user_id) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<StatusOr<std::vector<AddNodesResult>>> AddNodes(
      std::vector<AddNodesItem> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<StatusOr<std::vector<StatusCode>>> DeleteNodes(
      std::vector<DeleteNodesItem> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<StatusOr<std::vector<StatusCode>>> AddReferences(
      std::vector<AddReferencesItem> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  Awaitable<StatusOr<std::vector<StatusCode>>> DeleteReferences(
      std::vector<DeleteReferencesItem> inputs) override {
    co_return Status{StatusCode::Bad};
  }

  base::Time timestamp = ParseTime("2026-04-22 09:01:00");
  Variant read_value = 123.0;
  int read_count = 0;
  ServiceContext last_read_context;
  std::vector<ReadValueId> last_read_inputs;
};

class ServerRuntimeContractTestBase {
 public:
  base::Time now_ = ParseTime("2026-04-22 09:00:00");
  const NodeId expected_user_id_ = NumericNode(700, 5);
  TestExecutor executor_;
  const AnyExecutor any_executor_ = executor_;
  testing::StrictMock<MockAttributeService> attribute_service_;
  testing::StrictMock<MockViewService> view_service_;
  testing::StrictMock<MockHistoryService> history_service_;
  testing::StrictMock<MockMethodService> method_service_;
  testing::StrictMock<MockNodeManagementService>
      node_management_service_;
  TestMonitoredItemService monitored_item_service_;
  ServerSessionManager session_manager_{{
      .authenticator = MakeCoroutineAuthenticator(
          [this](LocalizedText user_name, LocalizedText password)
              -> Awaitable<StatusOr<AuthenticationResult>> {
            EXPECT_EQ(user_name, LocalizedText{u"operator"});
            EXPECT_EQ(password, LocalizedText{u"secret"});
            co_return AuthenticationResult{.user_id = expected_user_id_,
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
                  .attribute_id = AttributeId::DisplayName}}};
  EXPECT_CALL(fixture.attribute_service_, Read(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](ServiceContext context,
              std::shared_ptr<const std::vector<ReadValueId>> inputs)
              -> Awaitable<StatusOr<std::vector<DataValue>>> {
            EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
            EXPECT_THAT(*inputs, testing::ElementsAre(request.inputs[0]));
            co_return std::vector{DataValue{
                LocalizedText{u"Pump"}, {}, fixture.now_, fixture.now_}};
          }));

  const auto response =
      fixture.template HandleResponse<ReadResponse>(connection, request);
  EXPECT_EQ(response.status.code(), StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value,
            Variant{LocalizedText{u"Pump"}});
}

template <typename Fixture>
void ExpectRoutesWriteRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  WriteRequest request;
  request.inputs.push_back(WriteValue{
      .node_id = NumericNode(2),
      .attribute_id = AttributeId::Value,
      .value = Variant{17.5},
  });
  EXPECT_CALL(fixture.attribute_service_, Write(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](ServiceContext context,
              std::shared_ptr<const std::vector<WriteValue>> inputs)
              -> Awaitable<StatusOr<std::vector<StatusCode>>> {
            EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
            EXPECT_EQ(inputs->size(), 1u);
            if (inputs->size() != 1u) {
              co_return Status{StatusCode::Bad};
            }
            EXPECT_EQ((*inputs)[0].node_id, request.inputs[0].node_id);
            EXPECT_EQ((*inputs)[0].attribute_id,
                      request.inputs[0].attribute_id);
            EXPECT_EQ((*inputs)[0].value, request.inputs[0].value);
            co_return std::vector<StatusCode>{
                StatusCode::Good_Manual};
          }));

  const auto response =
      fixture.template HandleResponse<WriteResponse>(connection, request);
  EXPECT_EQ(response.status.code(), StatusCode::Good);
  EXPECT_THAT(response.results,
              testing::ElementsAre(static_cast<StatusCode>(
                  StatusCode::Good_Manual)));
}

template <typename Fixture>
void ExpectRoutesCallRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  CallRequest request{
      .methods = {{.object_id = NumericNode(301),
                   .method_id = NumericNode(302),
                   .arguments = {Variant{7.0},
                                 Variant{std::string{"mode"}}}}}};
  EXPECT_CALL(fixture.method_service_,
              Call(request.methods[0].object_id, request.methods[0].method_id,
                   request.methods[0].arguments, fixture.expected_user_id_))
      .WillOnce(testing::Invoke([](NodeId, NodeId,
                                   std::vector<Variant>, NodeId) {
        return MakeMethodCallResult(StatusCode::Good);
      }));

  const auto response =
      fixture.template HandleResponse<CallResponse>(connection, request);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].status.code(), StatusCode::Good);
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
      .WillOnce(testing::Invoke([&](HistoryReadRawDetails details)
                                    -> Awaitable<HistoryReadRawResult> {
        EXPECT_TRUE(details.node_id == request.details.node_id);
        EXPECT_EQ(details.from, from);
        EXPECT_EQ(details.to, to);
        EXPECT_EQ(details.max_count, 3u);
        co_return HistoryReadRawResult{
            .status = StatusCode::Good,
            .values = {DataValue{
                Variant{12.5}, {}, fixture.now_, fixture.now_}},
            .continuation_point = {1, 2, 3},
        };
      }));

  const auto response = fixture.template HandleResponse<HistoryReadRawResponse>(
      connection, request);
  EXPECT_EQ(response.result.status.code(), StatusCode::Good);
  ASSERT_EQ(response.result.values.size(), 1u);
  EXPECT_EQ(response.result.values[0].value, Variant{12.5});
  EXPECT_EQ(response.result.continuation_point, (ByteString{1, 2, 3}));
}

template <typename Fixture>
void ExpectHistoryReadRawRejectsInvalidTimeRange(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  // No start time, no end time, and no continuation point: the raw-read details
  // are invalid, so the handler must answer Bad_HistoryOperationInvalid without
  // ever calling the (strict) history service mock.
  const HistoryReadRawRequest request{
      .details = {.node_id = NumericNode(401), .max_count = 3}};
  const auto response = fixture.template HandleResponse<HistoryReadRawResponse>(
      connection, request);
  EXPECT_EQ(response.result.status.code(),
            StatusCode::Bad_HistoryOperationInvalid);
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
          [&](NodeId node_id, base::Time actual_from,
              base::Time actual_to,
              EventFilter) -> Awaitable<HistoryReadEventsResult> {
            EXPECT_EQ(node_id, request.details.node_id);
            EXPECT_EQ(actual_from, from);
            EXPECT_EQ(actual_to, to);
            Event event;
            event.event_id = 99;
            event.time = fixture.now_;
            event.receive_time = fixture.now_;
            event.node_id = NumericNode(403);
            event.message = LocalizedText{u"alarm"};
            co_return HistoryReadEventsResult{
                .status = StatusCode::Good,
                .events = {std::move(event)},
            };
          }));

  const auto response =
      fixture.template HandleResponse<HistoryReadEventsResponse>(connection,
                                                                 request);
  EXPECT_EQ(response.result.status.code(), StatusCode::Good);
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
                 .target_node_id = ExpandedNodeId{NumericNode(507)}}}};
  DeleteReferencesRequest delete_references{
      .items = {{.source_node_id = NumericNode(508),
                 .reference_type_id = NumericNode(509),
                 .target_node_id = ExpandedNodeId{NumericNode(510)}}}};

  EXPECT_CALL(fixture.node_management_service_, AddNodes(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<AddNodesItem> items)
              -> Awaitable<
                  StatusOr<std::vector<AddNodesResult>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return Status{StatusCode::Bad};
            }
            EXPECT_EQ(items[0].requested_id, add_nodes.items[0].requested_id);
            EXPECT_EQ(items[0].parent_id, add_nodes.items[0].parent_id);
            EXPECT_EQ(items[0].type_definition_id,
                      add_nodes.items[0].type_definition_id);
            co_return std::vector{AddNodesResult{
                .status_code = StatusCode::Good,
                .added_node_id = NumericNode(511),
            }};
          }));
  EXPECT_CALL(fixture.node_management_service_, DeleteNodes(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<DeleteNodesItem> items)
              -> Awaitable<StatusOr<std::vector<StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return Status{StatusCode::Bad};
            }
            EXPECT_EQ(items[0].node_id, delete_nodes.items[0].node_id);
            EXPECT_TRUE(items[0].delete_target_references);
            co_return std::vector{StatusCode::Good,
                                  StatusCode::Bad_WrongNodeId};
          }));
  EXPECT_CALL(fixture.node_management_service_, AddReferences(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<AddReferencesItem> items)
              -> Awaitable<StatusOr<std::vector<StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return Status{StatusCode::Bad};
            }
            EXPECT_EQ(items[0].source_node_id,
                      add_references.items[0].source_node_id);
            EXPECT_EQ(items[0].reference_type_id,
                      add_references.items[0].reference_type_id);
            EXPECT_EQ(items[0].target_node_id,
                      add_references.items[0].target_node_id);
            co_return std::vector{StatusCode::Good,
                                  StatusCode::Bad_WrongTargetId};
          }));
  EXPECT_CALL(fixture.node_management_service_, DeleteReferences(testing::_))
      .WillOnce(testing::Invoke(
          [&](std::vector<DeleteReferencesItem> items)
              -> Awaitable<StatusOr<std::vector<StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return Status{StatusCode::Bad};
            }
            EXPECT_EQ(items[0].source_node_id,
                      delete_references.items[0].source_node_id);
            EXPECT_EQ(items[0].reference_type_id,
                      delete_references.items[0].reference_type_id);
            EXPECT_EQ(items[0].target_node_id,
                      delete_references.items[0].target_node_id);
            co_return Status{StatusCode::Bad_Disconnected};
          }));

  const auto add_nodes_response =
      fixture.template HandleResponse<AddNodesResponse>(connection, add_nodes);
  EXPECT_EQ(add_nodes_response.status.code(), StatusCode::Good);
  ASSERT_EQ(add_nodes_response.results.size(), 1u);
  EXPECT_EQ(add_nodes_response.results[0].status_code, StatusCode::Good);
  EXPECT_EQ(add_nodes_response.results[0].added_node_id, NumericNode(511));

  const auto delete_nodes_response =
      fixture.template HandleResponse<DeleteNodesResponse>(connection,
                                                           delete_nodes);
  EXPECT_EQ(delete_nodes_response.status.code(), StatusCode::Good);
  EXPECT_THAT(delete_nodes_response.results,
              testing::ElementsAre(StatusCode::Good,
                                   StatusCode::Bad_WrongNodeId));

  const auto add_references_response =
      fixture.template HandleResponse<AddReferencesResponse>(connection,
                                                             add_references);
  EXPECT_EQ(add_references_response.status.code(), StatusCode::Good);
  EXPECT_THAT(add_references_response.results,
              testing::ElementsAre(StatusCode::Good,
                                   StatusCode::Bad_WrongTargetId));

  const auto delete_references_response =
      fixture.template HandleResponse<DeleteReferencesResponse>(
          connection, delete_references);
  EXPECT_EQ(delete_references_response.status.code(),
            StatusCode::Bad_Disconnected);
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
  ASSERT_EQ(subscription.status.code(), StatusCode::Good);

  const auto create_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          first_connection,
          CreateMonitoredItemsRequest{
              .subscription_id = subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(11),
                                       .attribute_id =
                                           AttributeId::Value},
                   .requested_parameters = {.client_handle = 44,
                                            .sampling_interval_ms = 0,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  ASSERT_EQ(create_items.status.code(), StatusCode::Good);
  ASSERT_EQ(fixture.monitored_item_service_.items.size(), 1u);

  fixture.monitored_item_service_.items[0]->NotifyDataChange(
      DataValue{Variant{12.5}, {}, fixture.now_, fixture.now_});
  fixture.Detach(first_connection);
  EXPECT_FALSE(first_connection.authentication_token.has_value());

  typename Fixture::ConnectionState second_connection;
  const auto resumed = fixture.template HandleResponse<ActivateSessionResponse>(
      second_connection, ActivateSessionRequest{
                             .session_id = session_id,
                             .authentication_token = authentication_token,
                         });
  EXPECT_EQ(resumed.status.code(), StatusCode::Good);
  EXPECT_TRUE(resumed.resumed);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(100);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      second_connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), StatusCode::Good);
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
  ASSERT_EQ(created_subscription.status.code(), StatusCode::Good);

  const auto created_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          source_connection,
          CreateMonitoredItemsRequest{
              .subscription_id = created_subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(21),
                                       .attribute_id =
                                           AttributeId::Value},
                   .requested_parameters = {.client_handle = 55,
                                            .sampling_interval_ms = 0,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  ASSERT_EQ(created_items.status.code(), StatusCode::Good);
  ASSERT_EQ(fixture.monitored_item_service_.items.size(), 1u);

  fixture.monitored_item_service_.items[0]->NotifyDataChange(
      DataValue{Variant{77.0}, {}, fixture.now_, fixture.now_});

  typename Fixture::ConnectionState target_connection;
  fixture.CreateAndActivate(target_connection);
  const auto transferred =
      fixture.template HandleResponse<TransferSubscriptionsResponse>(
          target_connection,
          TransferSubscriptionsRequest{
              .subscription_ids = {created_subscription.subscription_id},
              .send_initial_values = true});
  EXPECT_EQ(transferred.results,
            (std::vector<StatusCode>{StatusCode::Good}));

  // The source session no longer owns any subscriptions, so its Publish is
  // answered with Bad_NoSubscription (OPC UA Part 4 §5.13.5).
  const auto source_publish = fixture.template HandleResponse<PublishResponse>(
      source_connection, PublishRequest{});
  EXPECT_EQ(source_publish.status.code(),
            StatusCode::Bad_NoSubscription);

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
  EXPECT_EQ(closed.status.code(), StatusCode::Good);
  EXPECT_FALSE(connection.authentication_token.has_value());

  const auto status = fixture.ReadStatus(
      connection,
      ReadRequest{.inputs = {{.node_id = NumericNode(31),
                              .attribute_id = AttributeId::Value}}});
  EXPECT_EQ(status, StatusCode::Bad_SessionIsLoggedOff);
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
  EXPECT_EQ(status, StatusCode::Bad_SessionIsLoggedOff);
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
  EXPECT_EQ(status, StatusCode::Bad_SessionIsLoggedOff);
}

template <typename Fixture>
void ExpectBrowseAndBrowseNextUseSessionScopedContinuationPoints(
    Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  EXPECT_CALL(fixture.view_service_, Browse(testing::_, testing::_))
      .WillOnce(testing::Invoke(
          [&](ServiceContext context,
              std::vector<BrowseDescription> inputs)
              -> Awaitable<StatusOr<std::vector<BrowseResult>>> {
            EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u) {
              co_return Status{StatusCode::Bad};
            }
            co_return std::vector<BrowseResult>{BrowseResult{
                .status_code = StatusCode::Good,
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
                                .direction = BrowseDirection::Both,
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
            StatusCode::Bad_WrongIndex);

  const auto browse_next = fixture.template HandleResponse<BrowseNextResponse>(
      connection, BrowseNextRequest{.continuation_points = {
                                        browse.results[0].continuation_point}});
  ASSERT_EQ(browse_next.results.size(), 1u);
  EXPECT_EQ(browse_next.results[0].status_code, StatusCode::Good);
  ASSERT_EQ(browse_next.results[0].references.size(), 1u);
  EXPECT_EQ(browse_next.results[0].references[0].node_id, NumericNode(906));
  EXPECT_TRUE(browse_next.results[0].continuation_point.empty());

  const auto invalid = fixture.template HandleResponse<BrowseNextResponse>(
      connection, BrowseNextRequest{.continuation_points = {
                                        browse.results[0].continuation_point}});
  ASSERT_EQ(invalid.results.size(), 1u);
  EXPECT_EQ(invalid.results[0].status_code, StatusCode::Bad_WrongIndex);
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
  ASSERT_EQ(created_subscription.status.code(), StatusCode::Good);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(300);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), StatusCode::Good);
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
  ASSERT_EQ(created_subscription.status.code(), StatusCode::Good);

  const auto create_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          connection,
          CreateMonitoredItemsRequest{
              .subscription_id = created_subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(51),
                                       .attribute_id =
                                           AttributeId::Value},
                   .requested_parameters = {.client_handle = 88,
                                            .sampling_interval_ms = 0,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  ASSERT_EQ(create_items.status.code(), StatusCode::Good);
  ASSERT_EQ(fixture.monitored_item_service_.items.size(), 1u);

  fixture.monitored_item_service_.items[0]->NotifyDataChange(
      DataValue{Variant{42.5}, {}, fixture.now_, fixture.now_});
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
  EXPECT_EQ(publish.status.code(), StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, created_subscription.subscription_id);
  EXPECT_EQ(publish.available_sequence_numbers,
            (std::vector<UInt32>{1u}));
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
  EXPECT_EQ(republish.status.code(), StatusCode::Good);
  EXPECT_EQ(republish.notification_message, publish.notification_message);

  fixture.now_ = fixture.now_ + base::TimeDelta::FromMilliseconds(300);
  const auto ack_publish = fixture.template HandleResponse<PublishResponse>(
      connection,
      PublishRequest{
          .subscription_acknowledgements = {{
              .subscription_id = created_subscription.subscription_id,
              .sequence_number = publish.notification_message.sequence_number,
          }}});
  EXPECT_EQ(ack_publish.status.code(), StatusCode::Good);
  EXPECT_EQ(ack_publish.results,
            (std::vector<StatusCode>{StatusCode::Good}));
  EXPECT_TRUE(ack_publish.available_sequence_numbers.empty());

  const auto after_ack = fixture.template HandleResponse<RepublishResponse>(
      connection,
      RepublishRequest{.subscription_id = created_subscription.subscription_id,
                       .retransmit_sequence_number =
                           publish.notification_message.sequence_number});
  EXPECT_EQ(after_ack.status.code(),
            StatusCode::Bad_MessageNotAvailable);
}

}  // namespace opcua::test
