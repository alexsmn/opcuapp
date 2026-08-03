#include "opcua/server/service_handler.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/services/service_context.h"
#include "opcua/types/co_result.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace testing;

namespace opcua {
namespace {

// The Read and Browse dispatch paths: that a request's operations reach the
// application callback unchanged, that the caller's ServiceContext travels with
// them, and that what the callback returns is mapped back onto the generated
// response. The sibling suites cover different ground — service_handler_call
// pins the Call result plumbing, service_handler_trace pins the completion
// logs — so this is the only compiled coverage of the forward-and-map contract
// for the two most heavily used services.

constexpr NamespaceIndex kUserNamespace = 5;
constexpr NamespaceIndex kNodeNamespace = 2;

NodeId NumericNode(NumericId id, NamespaceIndex ns = kNodeNamespace) {
  return {id, ns};
}

const NodeId& UserId() {
  static const NodeId kUserId = NumericNode(700, kUserNamespace);
  return kUserId;
}

// A handler whose session is activated as UserId(), so a callback can assert
// the rights context it was handed.
ServiceHandler MakeHandler(ServiceCallbacks callbacks) {
  return ServiceHandler{ServiceHandlerContext{
      .callbacks = std::move(callbacks),
      .service_context = ServiceContext{}.with_user_id(UserId()),
  }};
}

TEST(ServiceHandlerTest, ReadForwardsOperationsWithTheCallersUserContext) {
  std::shared_ptr<const std::vector<ReadValueId>> seen_inputs;
  NodeId seen_user_id;

  ServiceCallbacks callbacks;
  callbacks.read = [&](ServiceContext context,
                       std::shared_ptr<const std::vector<ReadValueId>> inputs)
      -> CoStatusOr<std::vector<DataValue>> {
    seen_user_id = context.user_id();
    seen_inputs = std::move(inputs);
    co_return std::vector{
        DataValue{LocalizedText{u"Pump"}, {}, DateTime{}, DateTime{}}};
  };

  ua::ReadRequest request{
      .nodes_to_read = {ua::ReadValueId{
          .node_id = NumericNode(1),
          .attribute_id = static_cast<UInt32>(AttributeId::DisplayName)}}};

  TestExecutor executor;
  const auto response = WaitAwaitable(
      executor, MakeHandler(std::move(callbacks)).Handle(request));

  EXPECT_EQ(seen_user_id, UserId());
  ASSERT_NE(seen_inputs, nullptr);
  EXPECT_THAT(*seen_inputs, ElementsAre(ReadValueId{
                                .node_id = NumericNode(1),
                                .attribute_id = AttributeId::DisplayName}));

  const auto* read_response = std::get_if<ua::ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  EXPECT_EQ(read_response->response_header.service_result.code(),
            StatusCode::Good);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(read_response->results[0].value, Variant{LocalizedText{u"Pump"}});
}

TEST(ServiceHandlerTest, BrowseForwardsOperationsWithTheCallersUserContext) {
  std::vector<BrowseDescription> seen_inputs;
  NodeId seen_user_id;

  ServiceCallbacks callbacks;
  callbacks.browse = [&](ServiceContext context,
                         std::vector<BrowseDescription> inputs)
      -> CoStatusOr<std::vector<BrowseResult>> {
    seen_user_id = context.user_id();
    seen_inputs = std::move(inputs);
    co_return std::vector{BrowseResult{.status_code = StatusCode::Good,
                                       .references = {ReferenceDescription{
                                           .reference_type_id = NumericNode(35),
                                           .forward = true,
                                           .node_id = NumericNode(2)}}}};
  };

  ua::BrowseRequest request{
      .requested_max_references_per_node = 10,
      .nodes_to_browse = {ua::BrowseDescription{
          .node_id = NumericNode(1),
          .browse_direction = ua::BrowseDirection::Forward,
          .reference_type_id = NumericNode(35),
          .include_subtypes = true}}};

  TestExecutor executor;
  const auto response = WaitAwaitable(
      executor, MakeHandler(std::move(callbacks)).Handle(request));

  EXPECT_EQ(seen_user_id, UserId());
  ASSERT_EQ(seen_inputs.size(), 1u);
  EXPECT_EQ(seen_inputs[0].node_id, NumericNode(1));
  EXPECT_EQ(seen_inputs[0].direction, BrowseDirection::Forward);
  EXPECT_EQ(seen_inputs[0].reference_type_id, NumericNode(35));
  EXPECT_TRUE(seen_inputs[0].include_subtypes);

  const auto* browse_response = std::get_if<ua::BrowseResponse>(&response);
  ASSERT_NE(browse_response, nullptr);
  EXPECT_EQ(browse_response->response_header.service_result.code(),
            StatusCode::Good);
  ASSERT_EQ(browse_response->results.size(), 1u);
  ASSERT_EQ(browse_response->results[0].references.size(), 1u);
  EXPECT_EQ(browse_response->results[0].references[0].node_id, NumericNode(2));
  EXPECT_TRUE(browse_response->results[0].references[0].is_forward);
}

// A bad status on an individual Read operation is an operation-level result,
// not a service-level failure: it rides on the DataValue while the response
// header stays Good. Collapsing the two would make a single unknown node id
// look like a failed Read service call.
TEST(ServiceHandlerTest, AReadOperationFailureLeavesTheServiceCallGood) {
  ServiceCallbacks callbacks;
  callbacks.read = [](ServiceContext,
                      std::shared_ptr<const std::vector<ReadValueId>>)
      -> CoStatusOr<std::vector<DataValue>> {
    co_return std::vector{MakeReadError(StatusCode::Bad_NodeIdUnknown)};
  };

  TestExecutor executor;
  const auto response = WaitAwaitable(
      executor,
      MakeHandler(std::move(callbacks))
          .Handle(ua::ReadRequest{
              .nodes_to_read = {ua::ReadValueId{
                  .node_id = NumericNode(9999),
                  .attribute_id = static_cast<UInt32>(AttributeId::Value)}}}));

  const auto* read_response = std::get_if<ua::ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  EXPECT_EQ(read_response->response_header.service_result.code(),
            StatusCode::Good);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(Status(read_response->results[0].status_code).full_code(),
            0x80340000u);
}

// Node-management mutations answer per operation. The runtime contract pins
// this for AddNodes; the three remaining mutations need their own coverage
// because each has a separate response-building path, and DeleteReferences
// additionally shows the other half of the distinction — a callback that fails
// as a whole becomes a service-level status with no results at all.
TEST(ServiceHandlerTest, NodeMutationsPropagatePerOperationStatuses) {
  ServiceCallbacks callbacks;
  callbacks.delete_nodes = [](ServiceContext,
                              std::vector<DeleteNodesItem> items)
      -> CoStatusOr<std::vector<StatusCode>> {
    EXPECT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].node_id, NumericNode(60));
    EXPECT_TRUE(items[0].delete_target_references);
    co_return std::vector{StatusCode::Good, StatusCode::Bad_NodeIdUnknown};
  };
  callbacks.add_references = [](ServiceContext,
                                std::vector<AddReferencesItem> items)
      -> CoStatusOr<std::vector<StatusCode>> {
    EXPECT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].source_node_id, NumericNode(61));
    EXPECT_EQ(items[0].reference_type_id, NumericNode(62));
    EXPECT_EQ(items[0].target_node_id, ExpandedNodeId{NumericNode(63)});
    co_return std::vector{StatusCode::Good,
                          StatusCode::Bad_TargetNodeIdInvalid};
  };
  callbacks.delete_references = [](ServiceContext,
                                   std::vector<DeleteReferencesItem> items)
      -> CoStatusOr<std::vector<StatusCode>> {
    EXPECT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].source_node_id, NumericNode(64));
    EXPECT_EQ(items[0].reference_type_id, NumericNode(65));
    EXPECT_EQ(items[0].target_node_id, ExpandedNodeId{NumericNode(66)});
    co_return Status{StatusCode::Bad_NoCommunication};
  };

  TestExecutor executor;
  const ServiceHandler handler = MakeHandler(std::move(callbacks));

  auto response = WaitAwaitable(
      executor,
      handler.Handle(ua::DeleteNodesRequest{
          .nodes_to_delete = {ua::DeleteNodesItem{
              .node_id = NumericNode(60), .delete_target_references = true}}}));
  const auto* delete_nodes = std::get_if<ua::DeleteNodesResponse>(&response);
  ASSERT_NE(delete_nodes, nullptr);
  EXPECT_EQ(delete_nodes->response_header.service_result.code(),
            StatusCode::Good);
  EXPECT_THAT(delete_nodes->results,
              ElementsAre(StatusCode::Good, StatusCode::Bad_NodeIdUnknown));

  response = WaitAwaitable(
      executor, handler.Handle(ua::AddReferencesRequest{
                    .references_to_add = {ua::AddReferencesItem{
                        .source_node_id = NumericNode(61),
                        .reference_type_id = NumericNode(62),
                        .target_node_id = ExpandedNodeId{NumericNode(63)}}}}));
  const auto* add_references =
      std::get_if<ua::AddReferencesResponse>(&response);
  ASSERT_NE(add_references, nullptr);
  EXPECT_EQ(add_references->response_header.service_result.code(),
            StatusCode::Good);
  EXPECT_THAT(
      add_references->results,
      ElementsAre(StatusCode::Good, StatusCode::Bad_TargetNodeIdInvalid));

  response = WaitAwaitable(
      executor, handler.Handle(ua::DeleteReferencesRequest{
                    .references_to_delete = {ua::DeleteReferencesItem{
                        .source_node_id = NumericNode(64),
                        .reference_type_id = NumericNode(65),
                        .target_node_id = ExpandedNodeId{NumericNode(66)}}}}));
  const auto* delete_references =
      std::get_if<ua::DeleteReferencesResponse>(&response);
  ASSERT_NE(delete_references, nullptr);
  EXPECT_EQ(delete_references->response_header.service_result.code(),
            StatusCode::Bad_NoCommunication);
  EXPECT_TRUE(delete_references->results.empty());
}

}  // namespace
}  // namespace opcua
