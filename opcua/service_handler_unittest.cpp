#include "opcua/service_handler.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/scada/attribute_service_mock.h"
#include "opcua/scada/history_service_mock.h"
#include "opcua/scada/method_service_mock.h"
#include "opcua/scada/node_management_service_mock.h"
#include "opcua/scada/service_context.h"
#include "opcua/scada/view_service_mock.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace testing;

namespace opcua {
namespace {

opcua::scada::NodeId NumericNode(opcua::scada::NumericId id, opcua::scada::NamespaceIndex ns = 2) {
  return {id, ns};
}

TEST(ServiceHandlerCanonicalTest,
     HandleRead_UsesCanonicalCommonSurfaceAndUserContext) {
  StrictMock<opcua::scada::MockAttributeService> attribute_service;
  StrictMock<opcua::scada::MockViewService> view_service;
  StrictMock<opcua::scada::MockHistoryService> history_service;
  StrictMock<opcua::scada::MockMethodService> method_service;
  StrictMock<opcua::scada::MockNodeManagementService> node_management_service;
  opcua::TestExecutor executor;
  const auto user_id = NumericNode(700, 5);
  ServiceHandler handler{{attribute_service,
                          view_service,
                          history_service,
                          method_service,
                          node_management_service,
                          user_id}};

  ReadRequest request{
      .inputs = {{.node_id = NumericNode(1),
                  .attribute_id = opcua::scada::AttributeId::DisplayName}}};
  EXPECT_CALL(attribute_service, Read(_, _))
      .WillOnce(Invoke([&](opcua::scada::ServiceContext context,
                           std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> {
        EXPECT_EQ(context.user_id(), user_id);
        EXPECT_THAT(*inputs, ElementsAre(request.inputs[0]));
        co_return std::vector{opcua::scada::DataValue{opcua::scada::LocalizedText{u"Pump"},
                                               {},
                                               opcua::base::Time{},
                                               opcua::base::Time{}}};
      }));

  const auto response = opcua::WaitAwaitable(executor, handler.Handle(request));
  const auto* read_response = std::get_if<ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  EXPECT_EQ(read_response->status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(read_response->results[0].value,
            opcua::scada::Variant{opcua::scada::LocalizedText{u"Pump"}});
}

TEST(ServiceHandlerCanonicalTest,
     HandleBrowse_ForwardsInputsAndReturnsReferences) {
  StrictMock<opcua::scada::MockAttributeService> attribute_service;
  StrictMock<opcua::scada::MockViewService> view_service;
  StrictMock<opcua::scada::MockHistoryService> history_service;
  StrictMock<opcua::scada::MockMethodService> method_service;
  StrictMock<opcua::scada::MockNodeManagementService> node_management_service;
  opcua::TestExecutor executor;
  const auto user_id = NumericNode(700, 5);
  ServiceHandler handler{{attribute_service,
                          view_service,
                          history_service,
                          method_service,
                          node_management_service,
                          user_id}};

  BrowseRequest request{
      .requested_max_references_per_node = 10,
      .inputs = {{.node_id = NumericNode(1),
                  .direction = opcua::scada::BrowseDirection::Forward,
                  .reference_type_id = NumericNode(35),
                  .include_subtypes = true}}};
  EXPECT_CALL(view_service, Browse(_, _))
      .WillOnce(Invoke([&](opcua::scada::ServiceContext context,
                           std::vector<opcua::scada::BrowseDescription> inputs)
                           -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::BrowseResult>>> {
        EXPECT_EQ(context.user_id(), user_id);
        EXPECT_THAT(inputs, ElementsAre(request.inputs[0]));
        co_return std::vector{opcua::scada::BrowseResult{
            .status_code = opcua::scada::StatusCode::Good,
            .references = {opcua::scada::ReferenceDescription{
                .reference_type_id = NumericNode(35),
                .forward = true,
                .node_id = NumericNode(2)}}}};
      }));

  const auto response = opcua::WaitAwaitable(executor, handler.Handle(request));
  const auto* browse_response = std::get_if<BrowseResponse>(&response);
  ASSERT_NE(browse_response, nullptr);
  EXPECT_EQ(browse_response->status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(browse_response->results.size(), 1u);
  ASSERT_EQ(browse_response->results[0].references.size(), 1u);
  EXPECT_EQ(browse_response->results[0].references[0].node_id,
            NumericNode(2));
}

}  // namespace
}  // namespace opcua
