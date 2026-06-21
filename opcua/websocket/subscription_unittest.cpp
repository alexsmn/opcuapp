#include "opcua/server_subscription.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/standard_node_ids.h"
#include "opcua/scada/test/test_monitored_item.h"

#include <boost/json/parse.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <type_traits>

namespace opcua::ws {
namespace {

// The monitored-item subscription pump parks its read loop on an asio
// steady_timer; resuming it after a notification is pushed (or after an item is
// first wired up) needs the timer service to settle, which a single `Drain`
// does not guarantee. Spin the executor so async notification delivery
// completes before assertions.
inline void DrainPump(opcua::TestExecutor& executor) {
  for (int i = 0; i < 200; ++i) {
    Drain(executor);
    std::this_thread::yield();
  }
}

opcua::NodeId NumericNode(opcua::NumericId id, opcua::NamespaceIndex ns = 2) {
  return {id, ns};
}

opcua::base::Time ParseTime(std::string_view value) {
  opcua::base::Time result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

class TestMonitoredItemService : public opcua::scada::MonitoredItemService {
 public:
  std::shared_ptr<opcua::scada::MonitoredItem> CreateMonitoredItem(
      const opcua::ReadValueId& value_id,
      const opcua::MonitoringParameters& params) {
    created_value_ids.push_back(value_id);
    created_params.push_back(params);
    if (return_null_for_all_requests) {
      return nullptr;
    }
    auto item = std::make_shared<opcua::TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  opcua::StatusOr<std::unique_ptr<opcua::scada::MonitoredItemSubscription>>
  CreateSubscription(opcua::ServiceContext /*context*/,
                     opcua::scada::MonitoredItemSubscriptionOptions options) override {
    return opcua::scada::MakeItemFactorySubscription(
        [this](const opcua::ReadValueId& value_id,
               const opcua::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }

  bool return_null_for_all_requests = false;
  std::vector<opcua::ReadValueId> created_value_ids;
  std::vector<opcua::MonitoringParameters> created_params;
  std::vector<std::shared_ptr<opcua::TestMonitoredItem>> items;
};

TEST(SubscriptionTest, PublishesDataChangesAcknowledgesAndRepublishes) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 10:00:00");
  ServerSubscription subscription{17,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 17,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(101),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  EXPECT_EQ(create.results[0].status.code(), opcua::StatusCode::Good);
  // The backing item is created asynchronously on the executor.
  DrainPump(executor);
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  monitored_item_service.items[0]->NotifyDataChange(opcua::DataValue{
      opcua::Variant{12.5}, {}, start, ParseTime("2026-04-20 10:00:01")});
  DrainPump(executor);
  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(99))
          .has_value());
  const auto first_publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(first_publish.has_value());
  EXPECT_EQ(first_publish->subscription_id, 17u);
  EXPECT_EQ(first_publish->available_sequence_numbers,
            (std::vector<opcua::UInt32>{1u}));
  ASSERT_EQ(first_publish->notification_message.notification_data.size(), 1u);
  const auto* first_data = std::get_if<DataChangeNotification>(
      &first_publish->notification_message.notification_data[0]);
  ASSERT_NE(first_data, nullptr);
  ASSERT_EQ(first_data->monitored_items.size(), 1u);
  EXPECT_EQ(first_data->monitored_items[0].client_handle, 44u);
  EXPECT_EQ(first_data->monitored_items[0].value.value.get<double>(), 12.5);

  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{13.5},
                       {},
                       start + opcua::base::TimeDelta::FromMilliseconds(300),
                       ParseTime("2026-04-20 10:00:04")});
  DrainPump(executor);
  EXPECT_EQ(subscription.Acknowledge(std::vector<opcua::UInt32>{1}),
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  const auto second_publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(300));
  ASSERT_TRUE(second_publish.has_value());
  EXPECT_TRUE(second_publish->results.empty());
  EXPECT_EQ(second_publish->available_sequence_numbers,
            (std::vector<opcua::UInt32>{2u}));

  const auto republish_first = subscription.Republish(1);
  EXPECT_EQ(republish_first.status.code(),
            opcua::StatusCode::Bad_MessageNotAvailable);
  const auto republish_second = subscription.Republish(2);
  EXPECT_EQ(republish_second.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(republish_second.notification_message.sequence_number, 2u);
}

TEST(SubscriptionTest,
     ReportsSpecStatusesForUnknownMonitoredItemsAndUnknownSequenceNumbers) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 15:00:00");
  ServerSubscription subscription{33,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto modify = subscription.ModifyMonitoredItems(
      {.subscription_id = 33,
       .items_to_modify = {
           {.monitored_item_id = 999u,
            .requested_parameters = {.client_handle = 1,
                                     .sampling_interval_ms = 50,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(modify.results.size(), 1u);
  EXPECT_EQ(modify.results[0].status.code(),
            opcua::StatusCode::Bad_MonitoredItemIdInvalid);

  const auto set_mode = subscription.SetMonitoringMode(
      {.subscription_id = 33,
       .monitoring_mode = MonitoringMode::Reporting,
       .monitored_item_ids = {999u}});
  EXPECT_EQ(set_mode.results,
            (std::vector<opcua::StatusCode>{
                opcua::StatusCode::Bad_MonitoredItemIdInvalid}));

  const auto deleted = subscription.DeleteMonitoredItems(
      {.subscription_id = 33, .monitored_item_ids = {999u}});
  EXPECT_EQ(deleted.results,
            (std::vector<opcua::StatusCode>{
                opcua::StatusCode::Bad_MonitoredItemIdInvalid}));

  // Acknowledging an unknown sequence number is Bad_SequenceNumberUnknown per
  // OPC UA Part 4 v1.05 5.14.5 (Publish, Table 91); Republish of an
  // unavailable message is Bad_MessageNotAvailable per 5.14.6.
  EXPECT_EQ(subscription.Acknowledge(std::vector<opcua::UInt32>{77u}),
            (std::vector<opcua::StatusCode>{
                opcua::StatusCode::Bad_SequenceNumberUnknown}));
  EXPECT_EQ(subscription.Republish(77u).status.code(),
            opcua::StatusCode::Bad_MessageNotAvailable);
}

TEST(SubscriptionTest, GeneratesKeepAliveAndQueuesWhilePublishingDisabled) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 11:00:00");
  ServerSubscription subscription{19,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  ASSERT_FALSE(subscription.TryPublish(start).has_value());
  const auto keep_alive =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(keep_alive.has_value());
  EXPECT_EQ(keep_alive->notification_message.sequence_number, 1u);
  EXPECT_TRUE(keep_alive->notification_message.notification_data.empty());

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 19,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(201),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 88,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  DrainPump(executor);
  subscription.SetPublishingEnabled(false);
  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{77.0},
                       {},
                       start + opcua::base::TimeDelta::FromSeconds(1),
                       start + opcua::base::TimeDelta::FromSeconds(1)});
  DrainPump(executor);
  const auto disabled_keep_alive =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(1050));
  ASSERT_TRUE(disabled_keep_alive.has_value());
  EXPECT_EQ(disabled_keep_alive->notification_message.sequence_number, 1u);
  EXPECT_TRUE(
      disabled_keep_alive->notification_message.notification_data.empty());

  subscription.SetPublishingEnabled(true);
  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(1060))
          .has_value());
  const auto publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(1150));
  ASSERT_TRUE(publish.has_value());
  const auto* data = std::get_if<DataChangeNotification>(
      &publish->notification_message.notification_data[0]);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->monitored_items[0].value.value.get<double>(), 77.0);
}

TEST(SubscriptionTest, WaitsForPublishingIntervalBeforeSendingDataOrKeepAlive) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 11:30:00");
  ServerSubscription subscription{37,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 37,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(3701),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 37,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  DrainPump(executor);
  ASSERT_EQ(monitored_item_service.items.size(), 1u);

  EXPECT_FALSE(subscription.TryPublish(start).has_value());

  monitored_item_service.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{37.0}, {}, start, start});
  DrainPump(executor);
  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(99))
          .has_value());

  const auto data_publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(data_publish.has_value());
  const auto* data = std::get_if<DataChangeNotification>(
      &data_publish->notification_message.notification_data[0]);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->monitored_items[0].value.value.get<double>(), 37.0);

  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(399))
          .has_value());
  const auto keep_alive =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(400));
  ASSERT_TRUE(keep_alive.has_value());
  EXPECT_EQ(keep_alive->notification_message.sequence_number, 2u);
  EXPECT_TRUE(keep_alive->notification_message.notification_data.empty());
}

TEST(SubscriptionTest,
     ProjectsDefaultEventFieldsAndDropsOldQueuedEventsByQueueSize) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 12:00:00");
  ServerSubscription subscription{23,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 5,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const boost::json::value event_filter = boost::json::parse(R"({
    "Type":"EventFilter",
    "Body":{"SelectClauses":[
      {"BrowsePath":[{"Name":"EventId"}]},
      {"BrowsePath":[{"Name":"Message"}]},
      {"BrowsePath":[{"Name":"Severity"}]}
    ]}}
  )");
  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 23,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(2253, 0),
                                .attribute_id =
                                    static_cast<opcua::AttributeId>(12)},
            .requested_parameters = {.client_handle = 5,
                                     .sampling_interval_ms = 0,
                                     .filter = MonitoringFilter{event_filter},
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  DrainPump(executor);

  opcua::Event first_event;
  first_event.event_id = 11;
  first_event.event_type_id = NumericNode(2041, 0);
  first_event.node_id = NumericNode(3001);
  first_event.time = start;
  first_event.receive_time = ParseTime("2026-04-20 12:00:01");
  first_event.message = u"first";
  first_event.severity = 400;

  opcua::Event second_event = first_event;
  second_event.event_id = 22;
  second_event.message = u"second";
  second_event.severity = 700;

  monitored_item_service.items[0]->NotifyEvent(first_event);
  monitored_item_service.items[0]->NotifyEvent(second_event);
  DrainPump(executor);

  const auto publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(publish.has_value());
  const auto* events = std::get_if<EventNotificationList>(
      &publish->notification_message.notification_data[0]);
  ASSERT_NE(events, nullptr);
  // queue_size == 1 with discard_oldest keeps only the most recently queued
  // event. The event is projected onto the select clauses
  // ({EventId},{Message},{Severity}) at the data source and carried as a
  // standard EventFieldList, so the kept (second) event's real field values
  // survive end-to-end alongside the queue/drop mechanics.
  ASSERT_EQ(events->events.size(), 1u);
  EXPECT_EQ(events->events[0].client_handle, 5u);
  ASSERT_EQ(events->events[0].event_fields.size(), 3u);
  EXPECT_EQ(events->events[0].event_fields[0].get<opcua::UInt64>(), 22u);
  EXPECT_EQ(events->events[0].event_fields[1].get<opcua::LocalizedText>(),
            opcua::LocalizedText{u"second"});
  EXPECT_EQ(events->events[0].event_fields[2].get<opcua::UInt32>(), 700u);
}

TEST(SubscriptionTest, PassesRawEventFilterRestrictionsToMonitoredItemService) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 12:30:00");
  ServerSubscription subscription{41,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 5,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const boost::json::value event_filter = boost::json::parse(R"({
    "Type":"EventFilter",
    "OfType":["i=2130"],
    "ChildOf":["ns=2;s=Device1"],
    "Types":2,
    "Body":{"SelectClauses":[
      {"BrowsePath":[{"Name":"EventId"}]},
      {"BrowsePath":[{"Name":"Message"}]},
      {"BrowsePath":[{"Name":"Severity"}]}
    ]}}
  )");
  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 41,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(2253, 0),
                                .attribute_id =
                                    static_cast<opcua::AttributeId>(12)},
            .requested_parameters = {.client_handle = 6,
                                     .sampling_interval_ms = 0,
                                     .filter = MonitoringFilter{event_filter},
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  executor.Poll();
  ASSERT_EQ(monitored_item_service.created_params.size(), 1u);

  // The wire monitoring filter is forwarded to the service untouched; the
  // server no longer parses event routing constraints into a typed
  // EventFilter (that translation now lives in the SCADA bridge).
  const auto& forwarded_filter =
      monitored_item_service.created_params[0].filter;
  ASSERT_TRUE(forwarded_filter.has_value());
  const auto* raw_filter =
      std::get_if<boost::json::value>(&*forwarded_filter);
  ASSERT_NE(raw_filter, nullptr);
  EXPECT_EQ(*raw_filter, event_filter);
}

TEST(SubscriptionTest,
     RebindsModifiedItemsAndIgnoresLateCallbacksFromPreviousBinding) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  const auto start = ParseTime("2026-04-20 13:00:00");
  ServerSubscription subscription{29,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 5,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto create = subscription.CreateMonitoredItems(
      {.subscription_id = 29,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(401),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 9,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(create.results.size(), 1u);
  DrainPump(executor);
  ASSERT_EQ(monitored_item_service.items.size(), 1u);
  const auto old_item = monitored_item_service.items[0];

  const auto modify = subscription.ModifyMonitoredItems(
      {.subscription_id = 29,
       .items_to_modify = {
           {.monitored_item_id = create.results[0].monitored_item_id,
            .requested_parameters = {.client_handle = 99,
                                     .sampling_interval_ms = 50,
                                     .queue_size = 2,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(modify.results.size(), 1u);
  DrainPump(executor);
  ASSERT_EQ(monitored_item_service.items.size(), 2u);
  const auto new_item = monitored_item_service.items[1];

  old_item->NotifyDataChange(
      opcua::DataValue{opcua::Variant{1.0}, {}, start, start});
  DrainPump(executor);
  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(50))
          .has_value());

  new_item->NotifyDataChange(
      opcua::DataValue{opcua::Variant{2.0},
                       {},
                       start + opcua::base::TimeDelta::FromMilliseconds(200),
                       start + opcua::base::TimeDelta::FromMilliseconds(200)});
  DrainPump(executor);
  const auto publish =
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(100));
  ASSERT_TRUE(publish.has_value());
  const auto* data = std::get_if<DataChangeNotification>(
      &publish->notification_message.notification_data[0]);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->monitored_items[0].client_handle, 99u);

  const auto deleted = subscription.DeleteMonitoredItems(
      {.subscription_id = 29,
       .monitored_item_ids = {create.results[0].monitored_item_id}});
  EXPECT_EQ(deleted.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  new_item->NotifyDataChange(
      opcua::DataValue{opcua::Variant{3.0},
                       {},
                       start + opcua::base::TimeDelta::FromMilliseconds(400),
                       start + opcua::base::TimeDelta::FromMilliseconds(400)});
  DrainPump(executor);
  EXPECT_FALSE(
      subscription.TryPublish(start + opcua::base::TimeDelta::FromMilliseconds(200))
          .has_value());
}

TEST(SubscriptionTest,
     CreateMonitoredItemsReportsPreciseStatusForUnknownNodeAndBadAttribute) {
  TestMonitoredItemService monitored_item_service;
  opcua::TestExecutor executor;
  monitored_item_service.return_null_for_all_requests = true;
  const auto start = ParseTime("2026-04-20 14:00:00");

  ServerSubscription subscription{31,
                                  {.publishing_interval_ms = 100,
                                   .lifetime_count = 60,
                                   .max_keep_alive_count = 3,
                                   .max_notifications_per_publish = 0,
                                   .publishing_enabled = true,
                                   .priority = 0},
                                  executor,
                                  monitored_item_service,
                                  start};

  const auto unknown_node = subscription.CreateMonitoredItems(
      {.subscription_id = 31,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(999),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 1,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(unknown_node.results.size(), 1u);
  // A supported attribute on an unknown node is now accepted synchronously: the
  // LegacyMonitoredItemAdapter returns a placeholder item immediately and only
  // discovers the unknown node asynchronously while creating the backing
  // subscription, so Bad_WrongNodeId can no longer be surfaced here.
  EXPECT_EQ(unknown_node.results[0].status.code(), opcua::StatusCode::Good);
  executor.Poll();

  const auto bad_attribute = subscription.CreateMonitoredItems(
      {.subscription_id = 31,
       .items_to_create = {
           {.item_to_monitor = {.node_id = NumericNode(999),
                                .attribute_id =
                                    opcua::AttributeId::Description},
            .requested_parameters = {.client_handle = 2,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 1,
                                     .discard_oldest = true}}}});
  ASSERT_EQ(bad_attribute.results.size(), 1u);
  EXPECT_EQ(bad_attribute.results[0].status.code(),
            opcua::StatusCode::Bad_WrongAttributeId);
}

}  // namespace
}  // namespace opcua::ws
