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

scada::NodeId NumericNode(scada::NumericId id, scada::NamespaceIndex ns = 2) {
  return {id, ns};
}

TEST(ServiceHandlerCanonicalTest,
     HandleRead_UsesCanonicalCommonSurfaceAndUserContext) {
  StrictMock<scada::MockAttributeService> attribute_service;
  StrictMock<scada::MockViewService> view_service;
  StrictMock<scada::MockHistoryService> history_service;
  StrictMock<scada::MockMethodService> method_service;
  StrictMock<scada::MockNodeManagementService> node_management_service;
  TestExecutor executor;
  const auto user_id = NumericNode(700, 5);
  ServiceHandler handler{{attribute_service,
                          view_service,
                          history_service,
                          method_service,
                          node_management_service,
                          user_id}};

  ReadRequest request{
      .inputs = {{.node_id = NumericNode(1),
                  .attribute_id = scada::AttributeId::DisplayName}}};
  EXPECT_CALL(attribute_service, Read(_, _))
      .WillOnce(Invoke([&](scada::ServiceContext context,
                           std::shared_ptr<const std::vector<scada::ReadValueId>> inputs)
                           -> Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> {
        EXPECT_EQ(context.user_id(), user_id);
        EXPECT_THAT(*inputs, ElementsAre(request.inputs[0]));
        co_return std::vector{scada::DataValue{scada::LocalizedText{u"Pump"},
                                               {},
                                               base::Time{},
                                               base::Time{}}};
      }));

  const auto response = WaitAwaitable(executor, handler.Handle(request));
  const auto* read_response = std::get_if<ReadResponse>(&response);
  ASSERT_NE(read_response, nullptr);
  EXPECT_EQ(read_response->status.code(), scada::StatusCode::Good);
  ASSERT_EQ(read_response->results.size(), 1u);
  EXPECT_EQ(read_response->results[0].value,
            scada::Variant{scada::LocalizedText{u"Pump"}});
}

TEST(ServiceHandlerCanonicalTest,
     HandleBrowse_ForwardsInputsAndReturnsReferences) {
  StrictMock<scada::MockAttributeService> attribute_service;
  StrictMock<scada::MockViewService> view_service;
  StrictMock<scada::MockHistoryService> history_service;
  StrictMock<scada::MockMethodService> method_service;
  StrictMock<scada::MockNodeManagementService> node_management_service;
  TestExecutor executor;
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
                  .direction = scada::BrowseDirection::Forward,
                  .reference_type_id = NumericNode(35),
                  .include_subtypes = true}}};
  EXPECT_CALL(view_service, Browse(_, _))
      .WillOnce(Invoke([&](scada::ServiceContext context,
                           std::vector<scada::BrowseDescription> inputs)
                           -> Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>> {
        EXPECT_EQ(context.user_id(), user_id);
        EXPECT_THAT(inputs, ElementsAre(request.inputs[0]));
        co_return std::vector{scada::BrowseResult{
            .status_code = scada::StatusCode::Good,
            .references = {scada::ReferenceDescription{
                .reference_type_id = NumericNode(35),
                .forward = true,
                .node_id = NumericNode(2)}}}};
      }));

  const auto response = WaitAwaitable(executor, handler.Handle(request));
  const auto* browse_response = std::get_if<BrowseResponse>(&response);
  ASSERT_NE(browse_response, nullptr);
  EXPECT_EQ(browse_response->status.code(), scada::StatusCode::Good);
  ASSERT_EQ(browse_response->results.size(), 1u);
  ASSERT_EQ(browse_response->results[0].references.size(), 1u);
  EXPECT_EQ(browse_response->results[0].references[0].node_id,
            NumericNode(2));
}

}  // namespace
}  // namespace opcua
