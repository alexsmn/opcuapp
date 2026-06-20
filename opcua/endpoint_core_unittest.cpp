#include "opcua/endpoint_core.h"

#include "opcua/base/test/test_executor.h"
#include "opcua/scada/legacy_monitored_item_adapter.h"
#include "opcua/scada/monitored_item_service_mock.h"
#include "opcua/scada/monitoring_parameters.h"
#include "opcua/scada/test/test_monitored_item.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace testing;

namespace opcua {
namespace {

class EndpointCoreTest : public Test {
 protected:
  static scada::NodeId NumericNode(scada::NumericId id,
                                   scada::NamespaceIndex ns = 2) {
    return {id, ns};
  }

  StrictMock<scada::MockMonitoredItemService> monitored_item_service_;
  TestExecutor executor_;
  scada::LegacyMonitoredItemAdapter monitored_item_adapter_{
      executor_, monitored_item_service_};
};

TEST_F(EndpointCoreTest, MakeServiceContext_SetsRequestedUserId) {
  const auto user_id = NumericNode(42, 3);
  const auto context = MakeServiceContext(user_id);
  EXPECT_EQ(context.user_id(), user_id);
}

TEST_F(EndpointCoreTest, NormalizeReadResults_RewritesWrongNodeId) {
  auto results = NormalizeReadResults(
      {scada::MakeReadError(scada::StatusCode::Bad_WrongNodeId),
       scada::MakeReadError(scada::StatusCode::Bad_WrongAttributeId)});

  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(scada::Status(results[0].status_code).full_code(), 0x80340000u);
  EXPECT_EQ(results[1].status_code, scada::StatusCode::Bad_WrongAttributeId);
}

TEST_F(EndpointCoreTest, CreateMonitoredItem_ForwardsInputsAndReturnsItem) {
  const auto read_value_id = scada::ReadValueId{
      .node_id = NumericNode(50, 9), .attribute_id = scada::AttributeId::Value};
  scada::MonitoringParameters parameters;
  parameters.sampling_interval = base::TimeDelta::FromMilliseconds(250);
  parameters.queue_size = 3u;
  auto backing_item = std::make_shared<scada::TestMonitoredItem>();

  // The LegacyMonitoredItemAdapter creates the backing item lazily: the
  // service's CreateMonitoredItem is only invoked once the returned item is
  // subscribed and the executor is pumped.
  scada::ReadValueId forwarded_read_value_id;
  scada::MonitoringParameters forwarded_parameters;
  EXPECT_CALL(monitored_item_service_, CreateMonitoredItem(_, _))
      .WillOnce(Invoke([&](const scada::ReadValueId& actual_read_value_id,
                           const scada::MonitoringParameters& actual_parameters)
                           -> std::shared_ptr<scada::MonitoredItem> {
        forwarded_read_value_id = actual_read_value_id;
        forwarded_parameters = actual_parameters;
        return backing_item;
      }));

  auto created =
      CreateMonitoredItem(monitored_item_adapter_, read_value_id, parameters);

  // The helper accepts a supported attribute synchronously and returns a
  // (placeholder) item immediately.
  EXPECT_EQ(created.status, scada::StatusCode::Good);
  ASSERT_TRUE(created.monitored_item);

  std::optional<scada::DataValue> delivered;
  created.monitored_item->Subscribe(scada::DataChangeHandler{
      [&](const scada::DataValue& value) { delivered = value; }});

  // Drive the adapter's asynchronous subscription creation.
  executor_.Poll();

  EXPECT_EQ(forwarded_read_value_id, read_value_id);
  ASSERT_TRUE(forwarded_parameters.sampling_interval.has_value());
  EXPECT_EQ(*forwarded_parameters.sampling_interval,
            base::TimeDelta::FromMilliseconds(250));
  ASSERT_TRUE(forwarded_parameters.queue_size.has_value());
  EXPECT_EQ(*forwarded_parameters.queue_size, 3u);

  const scada::DataValue value{scada::Variant{scada::Int32{17}},
                               {},
                               scada::DateTime::Now(),
                               scada::DateTime::Now()};
  backing_item->NotifyDataChange(value);
  executor_.Poll();

  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(*delivered, value);
}

TEST_F(EndpointCoreTest,
       CreateMonitoredItem_TranslatesBadAttributeSynchronously) {
  const auto bad_attribute =
      scada::ReadValueId{.node_id = NumericNode(60, 9),
                         .attribute_id = scada::AttributeId::DisplayName};
  const auto unknown_node =
      scada::ReadValueId{.node_id = NumericNode(61, 9),
                         .attribute_id = scada::AttributeId::EventNotifier};
  const scada::MonitoringParameters parameters;

  // An unsupported attribute is rejected synchronously, before the adapter is
  // ever asked to create an item.
  auto bad_attribute_result =
      CreateMonitoredItem(monitored_item_adapter_, bad_attribute, parameters);
  EXPECT_EQ(bad_attribute_result.status,
            scada::StatusCode::Bad_WrongAttributeId);
  EXPECT_FALSE(bad_attribute_result.monitored_item);

  // A supported attribute (even on an unknown node) is now accepted
  // synchronously: the LegacyMonitoredItemAdapter returns a placeholder item
  // immediately and only discovers an unknown node asynchronously while
  // creating the backing subscription, so CreateMonitoredItem can no longer
  // surface Bad_WrongNodeId here.
  auto unknown_node_result =
      CreateMonitoredItem(monitored_item_adapter_, unknown_node, parameters);
  EXPECT_EQ(unknown_node_result.status, scada::StatusCode::Good);
  EXPECT_TRUE(unknown_node_result.monitored_item);
}

TEST_F(EndpointCoreTest,
       SubscribeMonitoredItemNotifications_UsesDataChangeHandlerForValueItems) {
  auto monitored_item = std::make_shared<scada::TestMonitoredItem>();
  const auto read_value_id = scada::ReadValueId{
      .node_id = NumericNode(70, 9), .attribute_id = scada::AttributeId::Value};
  std::optional<scada::DataValue> delivered;

  SubscribeMonitoredItemNotifications(
      read_value_id, monitored_item,
      [&](const scada::DataValue& value) { delivered = value; },
      [&](const scada::Status&, const std::any&) { FAIL(); });

  const scada::DataValue value{scada::Variant{scada::Int32{17}},
                               {},
                               scada::DateTime::Now(),
                               scada::DateTime::Now()};
  monitored_item->NotifyDataChange(value);

  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(*delivered, value);
}

TEST_F(EndpointCoreTest,
       SubscribeMonitoredItemNotifications_UsesEventHandlerForEventItems) {
  auto monitored_item = std::make_shared<scada::TestMonitoredItem>();
  const auto read_value_id =
      scada::ReadValueId{.node_id = NumericNode(71, 9),
                         .attribute_id = scada::AttributeId::EventNotifier};
  std::optional<scada::Status> delivered_status;
  std::optional<int> delivered_event_id;

  SubscribeMonitoredItemNotifications(
      read_value_id, monitored_item, [&](const scada::DataValue&) { FAIL(); },
      [&](const scada::Status& status, const std::any& event) {
        delivered_status = status;
        delivered_event_id = std::any_cast<int>(event);
      });

  monitored_item->NotifyEvent(42);

  ASSERT_TRUE(delivered_status.has_value());
  ASSERT_TRUE(delivered_event_id.has_value());
  EXPECT_EQ(delivered_status->code(), scada::StatusCode::Good);
  EXPECT_EQ(*delivered_event_id, 42);
}

TEST_F(EndpointCoreTest,
       DispatchMonitoredItemNotifications_ForwardsMatchingHandlerKinds) {
  const auto value_item = scada::ReadValueId{
      .node_id = NumericNode(72, 9), .attribute_id = scada::AttributeId::Value};
  const auto event_item =
      scada::ReadValueId{.node_id = NumericNode(73, 9),
                         .attribute_id = scada::AttributeId::EventNotifier};
  const scada::DataValue value{scada::Variant{scada::Int32{23}},
                               {},
                               scada::DateTime::Now(),
                               scada::DateTime::Now()};
  std::optional<scada::DataValue> delivered_value;
  std::optional<scada::Status> delivered_status;
  std::optional<int> delivered_event_id;

  const std::optional<scada::MonitoredItemHandler> data_handler =
      scada::DataChangeHandler{[&](const scada::DataValue& data_value) {
        delivered_value = data_value;
      }};
  const std::optional<scada::MonitoredItemHandler> event_handler =
      scada::EventHandler{
          [&](const scada::Status& status, const std::any& event) {
            delivered_status = status;
            delivered_event_id = std::any_cast<int>(event);
          }};

  EXPECT_TRUE(DispatchDataChangeNotification(value_item, data_handler, value));
  EXPECT_TRUE(DispatchEventNotification(event_item, event_handler,
                                        scada::StatusCode::Good, 91));
  EXPECT_FALSE(
      DispatchDataChangeNotification(event_item, event_handler, value));
  EXPECT_FALSE(DispatchEventNotification(value_item, data_handler,
                                         scada::StatusCode::Good, 91));

  ASSERT_TRUE(delivered_value.has_value());
  EXPECT_EQ(*delivered_value, value);
  ASSERT_TRUE(delivered_status.has_value());
  EXPECT_EQ(delivered_status->code(), scada::StatusCode::Good);
  ASSERT_TRUE(delivered_event_id.has_value());
  EXPECT_EQ(*delivered_event_id, 91);
}

TEST_F(EndpointCoreTest,
       ProjectEventFields_PreservesRequestedSelectClauseOrder) {
  scada::Event event;
  event.event_id = 11;
  event.event_type_id = NumericNode(501, 0);
  event.node_id = NumericNode(777, 4);
  event.time = scada::DateTime::Now();
  event.message = scada::LocalizedText{u"alarm"};
  event.severity = 900;

  const auto fields = ProjectEventFields(
      {{"Severity"}, {"Message"}, {"EventId"}, {"UnknownField"}}, event);

  ASSERT_EQ(fields.size(), 4u);
  EXPECT_EQ(fields[0].get<scada::UInt32>(), 900u);
  EXPECT_EQ(fields[1].get<scada::LocalizedText>(),
            scada::LocalizedText{u"alarm"});
  EXPECT_EQ(fields[2].get<scada::UInt64>(), 11u);
  EXPECT_TRUE(fields[3].is_null());
}

TEST_F(EndpointCoreTest, NormalizeEventFieldPaths_UsesDefaultsOnlyWhenEmpty) {
  const auto defaults = NormalizeEventFieldPaths({});
  EXPECT_EQ(defaults, DefaultEventFieldPaths());

  const std::vector<std::vector<std::string>> custom{
      {"Severity"},
      {"Message"},
  };
  EXPECT_EQ(NormalizeEventFieldPaths(custom), custom);
}

TEST_F(EndpointCoreTest, ParseAndBuildEventFilter_RoundTripsFieldPaths) {
  const auto filter = BuildEventFilter(std::vector<std::vector<std::string>>{
      {"Severity"},
      {"Message"},
  });
  EXPECT_EQ(ParseEventFilterFieldPaths(filter),
            (std::vector<std::vector<std::string>>{{"Severity"}, {"Message"}}));
}

TEST_F(EndpointCoreTest,
       DispatchEventFieldNotification_AssemblesAndForwardsModelChangeEvents) {
  const auto event_item =
      scada::ReadValueId{.node_id = NumericNode(74, 9),
                         .attribute_id = scada::AttributeId::EventNotifier};
  std::optional<scada::Status> delivered_status;
  std::optional<scada::ModelChangeEvent> delivered_event;

  const std::optional<scada::MonitoredItemHandler> event_handler =
      scada::EventHandler{
          [&](const scada::Status& status, const std::any& event) {
            delivered_status = status;
            delivered_event = std::any_cast<scada::ModelChangeEvent>(event);
          }};

  const auto fields = std::vector<scada::Variant>{
      scada::NodeId{scada::id::GeneralModelChangeEventType},
      NumericNode(500, 2),
      NumericNode(600, 3),
      static_cast<scada::UInt32>(scada::ModelChangeEvent::NodeAdded |
                                 scada::ModelChangeEvent::ReferenceAdded),
  };

  EXPECT_TRUE(
      DispatchEventFieldNotification(event_item, event_handler, fields));

  ASSERT_TRUE(delivered_status.has_value());
  ASSERT_TRUE(delivered_event.has_value());
  EXPECT_EQ(delivered_status->code(), scada::StatusCode::Good);
  EXPECT_EQ(delivered_event->node_id, NumericNode(500, 2));
  EXPECT_EQ(delivered_event->type_definition_id, NumericNode(600, 3));
  EXPECT_EQ(delivered_event->verb,
            static_cast<uint8_t>(scada::ModelChangeEvent::NodeAdded |
                                 scada::ModelChangeEvent::ReferenceAdded));
}

}  // namespace
}  // namespace opcua
