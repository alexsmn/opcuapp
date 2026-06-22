#include "opcua/server/service_handler.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/services/service_context.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace testing;

namespace opcua {
namespace {

opcua::NodeId NumericNode(opcua::NumericId id, opcua::NamespaceIndex ns = 2) {
  return {id, ns};
}

TEST(ServiceHandlerCanonicalTest,
     HandleRead_UsesCanonicalCommonSurfaceAndUserContext) {
  StrictMock<opcua::MockAttributeService> attribute_service;
  StrictMock<opcua::MockViewService> view_service;
  StrictMock<opcua::MockHistoryService> history_service;
  StrictMock<opcua::MockMethodService> method_service;
  StrictMock<opcua::MockNodeManagementService> node_management_service;
  opcua::TestExecutor executor;
  const auto user_id = NumericNode(700, 5);
  ServiceHandler handler{{attribute_service, view_service, history_service,
                          method_service, node_management_service, user_id}};

  ReadRequest request{
      .inputs = {{.node_id = NumericNode(1),
                  .attribute_id = opcua::AttributeId::DisplayName}}};
  EXPECT_CALL(attribute_service, Read(_, _))
      .WillOnce(Invoke(
          [&](opcua::ServiceContext context,
              std::shared_ptr<const std::vector<opcua::ReadValueId>> inputs)
              -> opcua::Awaitable<
                  opcua::StatusOr<std::vector<opcua::DataValue>>> {
            EXPECT_EQ(context.user_id(), user_id);
            EXPECT_THAT(*inputs, ElementsAre(request.inputs[0]));
            co_return std::vector{
                opcua::DataValue{opcua::LocalizedText{u"Pump"},
                                 {},
                                 opcua::base::Time{},
                                 opcua::base::Time{}}};
          }));

  const auto response = opcua::WaitAwaitable(executor, handler.Handle(request));
  const auto* read_response = std::get_if<ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  EXPECT_EQ(read_response->status.code(), opcua::StatusCode::Good);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(read_response->results[0].value,
            opcua::Variant{opcua::LocalizedText{u"Pump"}});
}

TEST(ServiceHandlerCanonicalTest,
     HandleBrowse_ForwardsInputsAndReturnsReferences) {
  StrictMock<opcua::MockAttributeService> attribute_service;
  StrictMock<opcua::MockViewService> view_service;
  StrictMock<opcua::MockHistoryService> history_service;
  StrictMock<opcua::MockMethodService> method_service;
  StrictMock<opcua::MockNodeManagementService> node_management_service;
  opcua::TestExecutor executor;
  const auto user_id = NumericNode(700, 5);
  ServiceHandler handler{{attribute_service, view_service, history_service,
                          method_service, node_management_service, user_id}};

  BrowseRequest request{
      .requested_max_references_per_node = 10,
      .inputs = {{.node_id = NumericNode(1),
                  .direction = opcua::BrowseDirection::Forward,
                  .reference_type_id = NumericNode(35),
                  .include_subtypes = true}}};
  EXPECT_CALL(view_service, Browse(_, _))
      .WillOnce(
          Invoke([&](opcua::ServiceContext context,
                     std::vector<opcua::BrowseDescription> inputs)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::BrowseResult>>> {
            EXPECT_EQ(context.user_id(), user_id);
            EXPECT_THAT(inputs, ElementsAre(request.inputs[0]));
            co_return std::vector{
                opcua::BrowseResult{.status_code = opcua::StatusCode::Good,
                                    .references = {opcua::ReferenceDescription{
                                        .reference_type_id = NumericNode(35),
                                        .forward = true,
                                        .node_id = NumericNode(2)}}}};
          }));

  const auto response = opcua::WaitAwaitable(executor, handler.Handle(request));
  const auto* browse_response = std::get_if<BrowseResponse>(&response);
  ASSERT_NE(browse_response, nullptr);
  EXPECT_EQ(browse_response->status.code(), opcua::StatusCode::Good);
  ASSERT_EQ(browse_response->results.size(), 1u);
  ASSERT_EQ(browse_response->results[0].references.size(), 1u);
  EXPECT_EQ(browse_response->results[0].references[0].node_id, NumericNode(2));
}

}  // namespace
}  // namespace opcua
