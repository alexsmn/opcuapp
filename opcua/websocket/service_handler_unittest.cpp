#include "opcua/service_handler.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/any_executor.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/scada/attribute_service_mock.h"
#include "opcua/scada/history_service_mock.h"
#include "opcua/scada/method_service_mock.h"
#include "opcua/scada/node_management_service_mock.h"
#include "opcua/scada/service_context.h"
#include "opcua/scada/view_service_mock.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <optional>

using namespace testing;

namespace opcua::ws {
namespace {

class ServiceHandlerTest : public Test {
 protected:
  static opcua::scada::NodeId NumericNode(opcua::scada::NumericId id,
                                   opcua::scada::NamespaceIndex ns = 2) {
    return {id, ns};
  }

  StrictMock<opcua::scada::MockAttributeService> attribute_service_;
  StrictMock<opcua::scada::MockViewService> view_service_;
  StrictMock<opcua::scada::MockHistoryService> history_service_;
  StrictMock<opcua::scada::MockMethodService> method_service_;
  StrictMock<opcua::scada::MockNodeManagementService> node_management_service_;
  opcua::TestExecutor executor_;
  const opcua::AnyExecutor any_executor_ = executor_;
  const opcua::scada::NodeId user_id_ = NumericNode(700, 3);
  ServiceHandler handler_{
      {attribute_service_,
       view_service_,
       history_service_,
       method_service_,
       node_management_service_,
       user_id_}};
};

TEST_F(ServiceHandlerTest,
       HandleReadWriteBrowseAndTranslate_UsesPhase0Services) {
  ReadRequest read_request{
      .inputs = {{.node_id = NumericNode(1),
                  .attribute_id = opcua::scada::AttributeId::DisplayName}}};
  WriteRequest write_request{
      .inputs = {{.node_id = NumericNode(2),
                  .attribute_id = opcua::scada::AttributeId::Value,
                  .value = opcua::scada::Variant{opcua::scada::Int32{7}},
                  .flags = opcua::scada::WriteFlags{}.set_select()}}};
  BrowseRequest browse_request{
      .inputs = {{.node_id = NumericNode(3),
                  .direction = opcua::scada::BrowseDirection::Forward,
                  .reference_type_id = NumericNode(33),
                  .include_subtypes = false}}};
  TranslateBrowsePathsRequest translate_request{
      .inputs = {{.node_id = NumericNode(4),
                  .relative_path = {{.reference_type_id = NumericNode(44),
                                     .inverse = true,
                                     .include_subtypes = false,
                                     .target_name = {"Leaf", 5}}}}}};

  EXPECT_CALL(attribute_service_, Read(_, _))
      .WillOnce(Invoke([&](opcua::scada::ServiceContext context,
                           std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> {
        EXPECT_EQ(context.user_id(), user_id_);
        EXPECT_EQ(inputs->size(), 1u);
        if (inputs->size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ((*inputs)[0], read_request.inputs[0]);
        co_return std::vector{opcua::scada::DataValue{opcua::scada::LocalizedText{u"Pump"},
                                               {},
                                               opcua::base::Time::Now(),
                                               opcua::base::Time::Now()}};
      }));
  EXPECT_CALL(attribute_service_, Write(_, _))
      .WillOnce(Invoke([&](opcua::scada::ServiceContext context,
                           std::shared_ptr<const std::vector<opcua::scada::WriteValue>> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::StatusCode>>> {
        EXPECT_EQ(context.user_id(), user_id_);
        EXPECT_EQ(inputs->size(), 1u);
        if (inputs->size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ((*inputs)[0], write_request.inputs[0]);
        co_return std::vector{opcua::scada::StatusCode::Good};
      }));
  EXPECT_CALL(view_service_, Browse(_, _))
      .WillOnce(Invoke([&](opcua::scada::ServiceContext context,
                           std::vector<opcua::scada::BrowseDescription> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::BrowseResult>>> {
        EXPECT_EQ(context.user_id(), user_id_);
        EXPECT_THAT(inputs, ElementsAre(browse_request.inputs[0]));
        co_return std::vector{opcua::scada::BrowseResult{
            .status_code = opcua::scada::StatusCode::Good,
            .references = {{.reference_type_id = NumericNode(34),
                            .forward = true,
                            .node_id = NumericNode(35)}}}};
      }));
  EXPECT_CALL(view_service_, TranslateBrowsePaths(_))
      .WillOnce(Invoke([&](std::vector<opcua::scada::BrowsePath> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::BrowsePathResult>>> {
        EXPECT_THAT(inputs, ElementsAre(translate_request.inputs[0]));
        co_return std::vector{opcua::scada::BrowsePathResult{
            .status_code = opcua::scada::StatusCode::Good,
            .targets = {{.target_id = opcua::scada::ExpandedNodeId{NumericNode(45)},
                         .remaining_path_index = 0}}}};
      }));

  auto response = opcua::WaitAwaitable(executor_, handler_.Handle(read_request));
  const auto* read_response = std::get_if<ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  EXPECT_EQ(read_response->status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(read_response->results[0].value,
            opcua::scada::Variant{opcua::scada::LocalizedText{u"Pump"}});

  response = opcua::WaitAwaitable(executor_, handler_.Handle(write_request));
  const auto* write_response = std::get_if<WriteResponse>(&response);
  ASSERT_NE(write_response, nullptr);
  EXPECT_EQ(write_response->status.code(), opcua::scada::StatusCode::Good);
  EXPECT_THAT(write_response->results, ElementsAre(opcua::scada::StatusCode::Good));

  response = opcua::WaitAwaitable(executor_, handler_.Handle(browse_request));
  const auto* browse_response = std::get_if<BrowseResponse>(&response);
  ASSERT_NE(browse_response, nullptr);
  EXPECT_EQ(browse_response->status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(browse_response->results.size(), 1u);
  EXPECT_THAT(browse_response->results[0].references,
              ElementsAre(opcua::scada::ReferenceDescription{
                  .reference_type_id = NumericNode(34),
                  .forward = true,
                  .node_id = NumericNode(35)}));

  response = opcua::WaitAwaitable(executor_, handler_.Handle(translate_request));
  const auto* translate_response =
      std::get_if<TranslateBrowsePathsResponse>(&response);
  ASSERT_NE(translate_response, nullptr);
  EXPECT_EQ(translate_response->status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(translate_response->results.size(), 1u);
  ASSERT_EQ(translate_response->results[0].targets.size(), 1u);
  EXPECT_EQ(translate_response->results[0].targets[0].target_id,
            opcua::scada::ExpandedNodeId{NumericNode(45)});
  EXPECT_EQ(translate_response->results[0].targets[0].remaining_path_index, 0u);
}

TEST_F(ServiceHandlerTest,
       HandleRead_MapsWrongNodeIdToBadNodeIdUnknown) {
  ReadRequest read_request{
      .inputs = {{.node_id = NumericNode(9999),
                  .attribute_id = opcua::scada::AttributeId::Value}}};

  EXPECT_CALL(attribute_service_, Read(_, _))
      .WillOnce(Invoke([&](opcua::scada::ServiceContext context,
                           std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> {
        EXPECT_EQ(context.user_id(), user_id_);
        EXPECT_EQ(inputs->size(), 1u);
        if (inputs->size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ((*inputs)[0], read_request.inputs[0]);
        co_return std::vector{
            opcua::scada::MakeReadError(opcua::scada::StatusCode::Bad_WrongNodeId)};
      }));

  const auto response = opcua::WaitAwaitable(executor_, handler_.Handle(read_request));
  const auto* read_response = std::get_if<ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(read_response->status.code(), opcua::scada::StatusCode::Good);
  EXPECT_EQ(opcua::scada::Status(read_response->results[0].status_code).full_code(),
            0x80340000u);
}

TEST_F(ServiceHandlerTest,
       HandleCall_ForwardsEachMethodWithSessionUserId) {
  CallRequest request{.methods = {
                          {.object_id = NumericNode(10),
                           .method_id = NumericNode(11),
                           .arguments = {opcua::scada::Variant{true}}},
                          {.object_id = NumericNode(12),
                           .method_id = NumericNode(13),
                           .arguments = {opcua::scada::Variant{opcua::scada::Int32{42}}}},
                      }};

  Sequence seq;
  EXPECT_CALL(method_service_,
              Call(request.methods[0].object_id,
                   request.methods[0].method_id,
                   request.methods[0].arguments,
                   user_id_))
      .WillOnce(Invoke([](opcua::scada::NodeId,
                          opcua::scada::NodeId,
                          std::vector<opcua::scada::Variant>,
                          opcua::scada::NodeId) {
        return opcua::scada::MakeMethodCallResult(opcua::scada::StatusCode::Good);
      }));
  EXPECT_CALL(method_service_,
              Call(request.methods[1].object_id,
                   request.methods[1].method_id,
                   request.methods[1].arguments,
                   user_id_))
      .WillOnce(Invoke([](opcua::scada::NodeId,
                          opcua::scada::NodeId,
                          std::vector<opcua::scada::Variant>,
                          opcua::scada::NodeId) {
        return opcua::scada::MakeMethodCallResult(
            opcua::scada::StatusCode::Bad_WrongCallArguments);
      }));

  auto response = opcua::WaitAwaitable(executor_, handler_.Handle(std::move(request)));
  const auto* call_response = std::get_if<CallResponse>(&response);
  ASSERT_NE(call_response, nullptr);
  ASSERT_EQ(call_response->results.size(), 2u);
  EXPECT_EQ(call_response->results[0].status.code(), opcua::scada::StatusCode::Good);
  EXPECT_EQ(call_response->results[1].status.code(),
            opcua::scada::StatusCode::Bad_WrongCallArguments);
}

TEST_F(ServiceHandlerTest, HandleHistoryReadRaw_PreservesResultPayload) {
  const auto node_id = NumericNode(30);
  const auto from = opcua::base::Time::Now() - opcua::base::TimeDelta::FromHours(1);
  const auto to = opcua::base::Time::Now();
  HistoryReadRawRequest request{
      .details = {.node_id = node_id, .from = from, .to = to, .max_count = 25}};

  EXPECT_CALL(history_service_, HistoryReadRaw(_))
      .WillOnce(Invoke([&](opcua::scada::HistoryReadRawDetails details)
                           -> opcua::Awaitable<opcua::scada::HistoryReadRawResult> {
        EXPECT_EQ(details.node_id, node_id);
        EXPECT_EQ(details.from, from);
        EXPECT_EQ(details.to, to);
        EXPECT_EQ(details.max_count, 25u);
        co_return opcua::scada::HistoryReadRawResult{
            .status = opcua::scada::StatusCode::Good,
            .values = {opcua::scada::DataValue{opcua::scada::Variant{12.5}, {}, to, to}},
            .continuation_point = {1, 2, 3},
        };
      }));

  auto response = opcua::WaitAwaitable(executor_, handler_.Handle(std::move(request)));
  const auto* raw_response = std::get_if<HistoryReadRawResponse>(&response);
  ASSERT_NE(raw_response, nullptr);
  EXPECT_EQ(raw_response->result.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(raw_response->result.values.size(), 1u);
  EXPECT_EQ(raw_response->result.values[0].value, opcua::scada::Variant{12.5});
  EXPECT_EQ(raw_response->result.continuation_point,
            (opcua::scada::ByteString{1, 2, 3}));
}

TEST_F(ServiceHandlerTest,
       HandleHistoryReadEvents_ForwardsFilterAndEvents) {
  opcua::scada::HistoryReadEventsDetails details{
      .node_id = NumericNode(40),
      .from = opcua::base::Time::Now() - opcua::base::TimeDelta::FromHours(4),
      .to = opcua::base::Time::Now(),
      .filter = {},
  };

  EXPECT_CALL(history_service_, HistoryReadEvents(_, _, _, _))
      .WillOnce(Invoke([&](opcua::scada::NodeId node_id,
                           opcua::base::Time from,
                           opcua::base::Time to,
                           opcua::scada::EventFilter)
                           -> opcua::Awaitable<opcua::scada::HistoryReadEventsResult> {
        EXPECT_EQ(node_id, details.node_id);
        EXPECT_EQ(from, details.from);
        EXPECT_EQ(to, details.to);
        opcua::scada::Event event;
        event.event_id = 99;
        event.time = opcua::base::Time::Now();
        event.receive_time = event.time;
        event.node_id = NumericNode(41);
        event.message = opcua::scada::LocalizedText{u"alarm"};
        co_return opcua::scada::HistoryReadEventsResult{
            .status = opcua::scada::StatusCode::Good,
            .events = {std::move(event)},
        };
      }));

  auto response = opcua::WaitAwaitable(
      executor_, handler_.Handle(HistoryReadEventsRequest{details}));
  const auto* events_response =
      std::get_if<HistoryReadEventsResponse>(&response);
  ASSERT_NE(events_response, nullptr);
  EXPECT_EQ(events_response->result.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(events_response->result.events.size(), 1u);
  EXPECT_EQ(events_response->result.events[0].event_id, 99u);
}

TEST_F(ServiceHandlerTest, HandleAddNodes_ForwardsBatchResults) {
  AddNodesRequest request{.items = {
                              {.requested_id = NumericNode(50),
                               .parent_id = NumericNode(51),
                               .type_definition_id = NumericNode(52)},
                          }};

  EXPECT_CALL(node_management_service_, AddNodes(_))
      .WillOnce(Invoke([&](std::vector<opcua::scada::AddNodesItem> items)
                           -> opcua::Awaitable<opcua::scada::StatusOr<
                               std::vector<opcua::scada::AddNodesResult>>> {
        EXPECT_EQ(items.size(), 1u);
        if (items.size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ(items[0].requested_id, NumericNode(50));
        EXPECT_EQ(items[0].parent_id, NumericNode(51));
        EXPECT_EQ(items[0].type_definition_id, NumericNode(52));
        co_return std::vector{opcua::scada::AddNodesResult{
            .status_code = opcua::scada::StatusCode::Good,
            .added_node_id = opcua::scada::NodeId{500, 4},
        }};
      }));

  auto response = opcua::WaitAwaitable(executor_, handler_.Handle(std::move(request)));
  const auto* add_nodes_response = std::get_if<AddNodesResponse>(&response);
  ASSERT_NE(add_nodes_response, nullptr);
  EXPECT_EQ(add_nodes_response->status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(add_nodes_response->results.size(), 1u);
  EXPECT_EQ(add_nodes_response->results[0].added_node_id, (opcua::scada::NodeId{500, 4}));
}

TEST_F(ServiceHandlerTest,
       HandleDeleteAndReferenceMutations_PropagatesStatuses) {
  DeleteNodesRequest delete_nodes_request{
      .items = {{.node_id = NumericNode(60), .delete_target_references = true}}};
  AddReferencesRequest add_references_request{
      .items = {{.source_node_id = NumericNode(61),
                 .reference_type_id = NumericNode(62),
                 .target_node_id = opcua::scada::ExpandedNodeId{NumericNode(63)}}}};
  DeleteReferencesRequest delete_references_request{
      .items = {{.source_node_id = NumericNode(64),
                 .reference_type_id = NumericNode(65),
                 .target_node_id = opcua::scada::ExpandedNodeId{NumericNode(66)}}}};

  EXPECT_CALL(node_management_service_, DeleteNodes(_))
      .WillOnce(Invoke([&](std::vector<opcua::scada::DeleteNodesItem> items)
                           -> opcua::Awaitable<opcua::scada::StatusOr<
                               std::vector<opcua::scada::StatusCode>>> {
        EXPECT_EQ(items.size(), 1u);
        if (items.size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ(items[0].node_id, NumericNode(60));
        EXPECT_TRUE(items[0].delete_target_references);
        co_return std::vector{opcua::scada::StatusCode::Good,
                              opcua::scada::StatusCode::Bad_WrongNodeId};
      }));
  EXPECT_CALL(node_management_service_, AddReferences(_))
      .WillOnce(Invoke([&](std::vector<opcua::scada::AddReferencesItem> items)
                           -> opcua::Awaitable<opcua::scada::StatusOr<
                               std::vector<opcua::scada::StatusCode>>> {
        EXPECT_EQ(items.size(), 1u);
        if (items.size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ(items[0].source_node_id, NumericNode(61));
        EXPECT_EQ(items[0].reference_type_id, NumericNode(62));
        EXPECT_EQ(items[0].target_node_id,
                  opcua::scada::ExpandedNodeId{NumericNode(63)});
        co_return std::vector{opcua::scada::StatusCode::Good,
                              opcua::scada::StatusCode::Bad_WrongTargetId};
      }));
  EXPECT_CALL(node_management_service_, DeleteReferences(_))
      .WillOnce(Invoke([&](std::vector<opcua::scada::DeleteReferencesItem> items)
                           -> opcua::Awaitable<opcua::scada::StatusOr<
                               std::vector<opcua::scada::StatusCode>>> {
        EXPECT_EQ(items.size(), 1u);
        if (items.size() != 1u) {
          co_return opcua::scada::Status{opcua::scada::StatusCode::Bad};
        }
        EXPECT_EQ(items[0].source_node_id, NumericNode(64));
        EXPECT_EQ(items[0].reference_type_id, NumericNode(65));
        EXPECT_EQ(items[0].target_node_id,
                  opcua::scada::ExpandedNodeId{NumericNode(66)});
        co_return opcua::scada::Status{opcua::scada::StatusCode::Bad_Disconnected};
      }));

  auto response =
      opcua::WaitAwaitable(executor_, handler_.Handle(std::move(delete_nodes_request)));
  const auto* delete_nodes_response =
      std::get_if<DeleteNodesResponse>(&response);
  ASSERT_NE(delete_nodes_response, nullptr);
  EXPECT_EQ(delete_nodes_response->status.code(), opcua::scada::StatusCode::Good);
  EXPECT_THAT(delete_nodes_response->results,
              ElementsAre(opcua::scada::StatusCode::Good,
                          opcua::scada::StatusCode::Bad_WrongNodeId));

  response = opcua::WaitAwaitable(executor_,
                           handler_.Handle(std::move(add_references_request)));
  const auto* add_references_response =
      std::get_if<AddReferencesResponse>(&response);
  ASSERT_NE(add_references_response, nullptr);
  EXPECT_EQ(add_references_response->status.code(), opcua::scada::StatusCode::Good);
  EXPECT_THAT(add_references_response->results,
              ElementsAre(opcua::scada::StatusCode::Good,
                          opcua::scada::StatusCode::Bad_WrongTargetId));

  response = opcua::WaitAwaitable(executor_,
                           handler_.Handle(std::move(delete_references_request)));
  const auto* delete_references_response =
      std::get_if<DeleteReferencesResponse>(&response);
  ASSERT_NE(delete_references_response, nullptr);
  EXPECT_EQ(delete_references_response->status.code(),
            opcua::scada::StatusCode::Bad_Disconnected);
  EXPECT_TRUE(delete_references_response->results.empty());
}

}  // namespace
}  // namespace opcua::ws
