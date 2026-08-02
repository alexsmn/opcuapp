#pragma once

// The shared server-runtime contract: behaviour every transport in front of
// `ServerRuntime` must exhibit, written once and run by each transport's own
// suite against its own fixture.
//
// A fixture supplies:
//   - `using ConnectionState = ...`            the transport's connection state
//   - `SessionIds CreateAndActivate(ConnectionState&)`
//   - `template <class Response, class Request>
//      Response HandleResponse(ConnectionState&, Request)`
//   - `void Detach(ConnectionState&)`
//   - `void Drain()`                           run pending coroutine work
//   - members `now_`, `expected_user_id_`, `services_`, `backing_states_`
//
// `DirectRuntimeFixture` below is the in-process implementation, used by
// session/server_runtime_unittest.cpp; a transport suite substitutes its own.

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/message.h"
#include "opcua/monitored/test/fake_monitored_item_subscription.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/session/server_runtime.h"
#include "opcua/session/server_session_manager.h"
#include "opcua/types/co_result.h"
#include "opcua/ua/ua_binary_codec.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace opcua::test {

using BackingStates =
    std::vector<std::shared_ptr<FakeMonitoredItemSubscription::State>>;

inline NodeId NumericNode(NumericId id, NamespaceIndex ns = 2) {
  return {id, ns};
}

inline DateTime ParseTime(std::string_view value) {
  DateTime result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

// The single data change carried by a PublishResponse, or nullopt when it
// carries none. Reads `notification_data` only after checking its size: an
// empty vector indexed at [0] is out of bounds, and `std::get_if` on that
// reference then reads a garbage variant index — which presents as a puzzling
// intermittent null check rather than a clean crash.
struct PublishedDataChange {
  UInt32 client_handle = 0;
  double value = 0;
};

inline std::optional<PublishedDataChange> SingleDataChange(
    const PublishResponse& response) {
  if (response.notification_message.notification_data.size() != 1)
    return std::nullopt;
  const auto* data_change = std::get_if<DataChangeNotification>(
      &response.notification_message.notification_data[0]);
  if (!data_change || data_change->monitored_items.size() != 1)
    return std::nullopt;
  double value = 0;
  if (!data_change->monitored_items[0].value.value.get(value))
    return std::nullopt;
  return PublishedDataChange{
      .client_handle = data_change->monitored_items[0].client_handle,
      .value = value};
}

// The application behind `ServiceCallbacks`: records what each callback was
// handed and answers with whatever the test scripted.
//
// This is deliberately a recording fake rather than a set of mocks. The
// callbacks are `std::function`s, so there is no interface to mock, and the
// assertions worth making are about the values that crossed the boundary —
// not about call shape.
class ScriptedServices {
 public:
  // Answers used when the matching script is unset.
  DataValue default_read_value{Variant{42.0}, {}, DateTime{}, DateTime{}};

  // --- scripts; set one to control that callback's answer ---
  std::function<StatusOr<std::vector<DataValue>>(ServiceContext,
                                                 std::vector<ReadValueId>)>
      read;
  std::function<StatusOr<std::vector<StatusCode>>(ServiceContext,
                                                  std::vector<WriteValue>)>
      write;
  std::function<StatusOr<
      CallResult>(NodeId, NodeId, std::vector<Variant>, ServiceContext)>
      call;
  std::function<StatusOr<std::vector<BrowseResult>>(
      ServiceContext,
      std::vector<BrowseDescription>)>
      browse;
  std::function<StatusOr<HistoryReadRawResult>(HistoryReadRawDetails)>
      history_read_raw;
  std::function<StatusOr<
      HistoryReadEventsResult>(NodeId, DateTime, DateTime, EventFilter)>
      history_read_events;
  std::function<StatusOr<std::vector<AddNodesResult>>(
      ServiceContext,
      std::vector<AddNodesItem>)>
      add_nodes;
  std::function<StatusOr<std::vector<BrowsePathResult>>(
      std::vector<BrowsePath>)>
      translate_browse_paths;
  std::function<StatusOr<std::vector<StatusCode>>(ServiceContext,
                                                  std::vector<DeleteNodesItem>)>
      delete_nodes;
  std::function<StatusOr<std::vector<StatusCode>>(
      ServiceContext,
      std::vector<AddReferencesItem>)>
      add_references;
  std::function<StatusOr<std::vector<StatusCode>>(
      ServiceContext,
      std::vector<DeleteReferencesItem>)>
      delete_references;

  // --- what the runtime actually handed the application ---
  int read_count = 0;
  ServiceContext last_read_context;
  std::vector<ReadValueId> last_read_inputs;

  int write_count = 0;
  ServiceContext last_write_context;
  std::vector<WriteValue> last_write_inputs;

  int call_count = 0;
  ServiceContext last_call_context;
  NodeId last_call_object_id;
  NodeId last_call_method_id;
  std::vector<Variant> last_call_arguments;

  int browse_count = 0;
  std::vector<BrowseDescription> last_browse_inputs;

  int history_read_raw_count = 0;
  std::optional<HistoryReadRawDetails> last_history_read_raw;

  int history_read_events_count = 0;
  NodeId last_history_events_node_id;

  int add_nodes_count = 0;
  std::vector<BrowsePath> last_translate_browse_paths_inputs;
  std::vector<AddNodesItem> last_add_nodes_items;
  std::vector<DeleteNodesItem> last_delete_nodes_items;
  std::vector<AddReferencesItem> last_add_references_items;
  std::vector<DeleteReferencesItem> last_delete_references_items;

  // Builds the callback set, with `create_subscription` handing out a fresh
  // FakeMonitoredItemSubscription::State per subscription into `states`.
  ServiceCallbacks MakeCallbacks(AnyExecutor executor,
                                 std::shared_ptr<BackingStates> states) {
    ServiceCallbacks callbacks;

    callbacks.read = [this](
                         ServiceContext context,
                         std::shared_ptr<const std::vector<ReadValueId>> inputs)
        -> CoStatusOr<std::vector<DataValue>> {
      ++read_count;
      last_read_context = context;
      last_read_inputs = *inputs;
      if (read)
        co_return read(std::move(context), *inputs);
      co_return std::vector<DataValue>(inputs->size(), default_read_value);
    };

    callbacks.write = [this](
                          ServiceContext context,
                          std::shared_ptr<const std::vector<WriteValue>> inputs)
        -> CoStatusOr<std::vector<StatusCode>> {
      ++write_count;
      last_write_context = context;
      last_write_inputs = *inputs;
      if (write)
        co_return write(std::move(context), *inputs);
      co_return std::vector<StatusCode>(inputs->size(), StatusCode::Good);
    };

    callbacks.call = [this](NodeId object_id, NodeId method_id,
                            std::vector<Variant> arguments,
                            ServiceContext context) -> CoStatusOr<CallResult> {
      ++call_count;
      last_call_context = context;
      last_call_object_id = object_id;
      last_call_method_id = method_id;
      last_call_arguments = arguments;
      if (call)
        co_return call(std::move(object_id), std::move(method_id),
                       std::move(arguments), std::move(context));
      co_return CallResult{};
    };

    callbacks.browse = [this](ServiceContext context,
                              std::vector<BrowseDescription> inputs)
        -> CoStatusOr<std::vector<BrowseResult>> {
      ++browse_count;
      last_browse_inputs = inputs;
      if (browse)
        co_return browse(std::move(context), std::move(inputs));
      co_return std::vector<BrowseResult>(inputs.size(), BrowseResult{});
    };

    callbacks.history_read_raw =
        [this](
            HistoryReadRawDetails details) -> CoStatusOr<HistoryReadRawResult> {
      ++history_read_raw_count;
      last_history_read_raw = details;
      if (history_read_raw)
        co_return history_read_raw(std::move(details));
      co_return HistoryReadRawResult{};
    };

    callbacks.history_read_events =
        [this](NodeId node_id, DateTime from, DateTime to,
               EventFilter filter) -> CoStatusOr<HistoryReadEventsResult> {
      ++history_read_events_count;
      last_history_events_node_id = node_id;
      if (history_read_events)
        co_return history_read_events(std::move(node_id), from, to,
                                      std::move(filter));
      co_return HistoryReadEventsResult{};
    };

    callbacks.add_nodes = [this](ServiceContext context,
                                 std::vector<AddNodesItem> items)
        -> CoStatusOr<std::vector<AddNodesResult>> {
      ++add_nodes_count;
      last_add_nodes_items = items;
      if (add_nodes)
        co_return add_nodes(std::move(context), std::move(items));
      std::vector<AddNodesResult> results;
      results.reserve(items.size());
      for (const auto& item : items) {
        results.push_back({.status_code = StatusCode::Good,
                           .added_node_id = item.requested_id});
      }
      co_return results;
    };

    callbacks.translate_browse_paths = [this](std::vector<BrowsePath> inputs)
        -> CoStatusOr<std::vector<BrowsePathResult>> {
      last_translate_browse_paths_inputs = inputs;
      if (translate_browse_paths)
        co_return translate_browse_paths(std::move(inputs));
      co_return std::vector<BrowsePathResult>(inputs.size());
    };

    callbacks.delete_nodes = [this](ServiceContext context,
                                    std::vector<DeleteNodesItem> items)
        -> CoStatusOr<std::vector<StatusCode>> {
      last_delete_nodes_items = items;
      if (delete_nodes)
        co_return delete_nodes(std::move(context), std::move(items));
      co_return std::vector<StatusCode>(items.size(), StatusCode::Good);
    };

    callbacks.add_references = [this](ServiceContext context,
                                      std::vector<AddReferencesItem> items)
        -> CoStatusOr<std::vector<StatusCode>> {
      last_add_references_items = items;
      if (add_references)
        co_return add_references(std::move(context), std::move(items));
      co_return std::vector<StatusCode>(items.size(), StatusCode::Good);
    };

    callbacks.delete_references =
        [this](ServiceContext context, std::vector<DeleteReferencesItem> items)
        -> CoStatusOr<std::vector<StatusCode>> {
      last_delete_references_items = items;
      if (delete_references)
        co_return delete_references(std::move(context), std::move(items));
      co_return std::vector<StatusCode>(items.size(), StatusCode::Good);
    };

    callbacks.create_subscription =
        FakeMonitoredItemSubscription::MakeCreateSubscriptionPerCall(
            std::move(executor), std::move(states));

    return callbacks;
  }
};

// The in-process fixture: requests go straight to ServerRuntime::Handle with
// no transport in between.
class DirectRuntimeFixture {
 public:
  using ConnectionState = opcua::ConnectionState;

  struct SessionIds {
    NodeId session_id;
    NodeId authentication_token;
  };

  DateTime now_ = ParseTime("2026-04-22 09:00:00");
  const NodeId expected_user_id_ = NumericNode(700, 5);
  TestExecutor executor_;
  ScriptedServices services_;
  std::shared_ptr<BackingStates> backing_states_ =
      std::make_shared<BackingStates>();

  ServerSessionManager session_manager_{{
      .authenticator = MakeCoroutineAuthenticator(
          [this](LocalizedText user_name,
                 LocalizedText password) -> CoStatusOr<AuthenticationResult> {
            EXPECT_EQ(user_name, LocalizedText{u"operator"});
            EXPECT_EQ(password, LocalizedText{u"secret"});
            co_return AuthenticationResult{.user_id = expected_user_id_,
                                           .multi_sessions = true};
          }),
      .now = [this] { return now_; },
  }};

  // Deferred work (a Publish held until its deadline) is captured rather than
  // scheduled. The default `post_delayed_task` posts a boost::asio
  // steady_timer, and TestExecutor is a bare execution_context with no timer
  // reactor — the timer would never fire and the request would never complete.
  // Capturing it lets `HandleResponse` let the wait "elapse" by advancing the
  // virtual clock, which is deterministic and needs no wall-clock time.
  std::vector<std::pair<Duration, std::function<void()>>> delayed_tasks_;

  ServerRuntime runtime_{ServerRuntimeContext{
      .executor = AnyExecutor{executor_},
      .session_manager = session_manager_,
      .callbacks =
          services_.MakeCallbacks(AnyExecutor{executor_}, backing_states_),
      .now = [this] { return now_; },
      .post_delayed_task =
          [this](Duration delay, std::function<void()> task) {
            delayed_tasks_.emplace_back(delay, std::move(task));
          },
  }};

  template <class Response, class Request>
  Response HandleResponse(ConnectionState& connection, Request request) {
    auto result = StartAwaitable<ResponseBody>(
        executor_,
        runtime_.Handle(connection, RequestBody{std::move(request)}));

    // Run pending work; if the request is waiting on a deadline, let that
    // deadline pass and run the continuation. Bounded so a genuinely stuck
    // request fails the test instead of hanging it.
    for (int step = 0; step < 16 && !result->done; ++step) {
      opcua::Drain(executor_);
      if (result->done || delayed_tasks_.empty())
        break;
      auto [delay, task] = std::move(delayed_tasks_.front());
      delayed_tasks_.erase(delayed_tasks_.begin());
      now_ = now_ + delay;
      task();
    }
    opcua::Drain(executor_);

    EXPECT_TRUE(result->done) << "request never completed";
    if (!result->done || !result->value.has_value())
      return Response{};
    auto* response = std::get_if<Response>(&*result->value);
    EXPECT_TRUE(response) << "unexpected response body";
    return response ? std::move(*response) : Response{};
  }

  SessionIds CreateAndActivate(ConnectionState& connection) {
    const auto created = HandleResponse<CreateSessionResponse>(
        connection, CreateSessionRequest{});
    EXPECT_EQ(created.status.code(), StatusCode::Good);
    const auto activated = HandleResponse<ActivateSessionResponse>(
        connection, ActivateSessionRequest{
                        .session_id = created.session_id,
                        .authentication_token = created.authentication_token,
                        .user_name = LocalizedText{u"operator"},
                        .password = LocalizedText{u"secret"},
                    });
    EXPECT_EQ(activated.status.code(), StatusCode::Good);
    return {created.session_id, created.authentication_token};
  }

  void Detach(ConnectionState& connection) { runtime_.Detach(connection); }

  void Drain() { opcua::Drain(executor_); }

  void Advance(int64_t ms) { now_ = now_ + Duration::FromMilliseconds(ms); }

  FakeMonitoredItemSubscription::State& backing(std::size_t index) const {
    return *backing_states_->at(index);
  }
};

// Creates a subscription with one monitored item on `connection`, binds it,
// and returns the subscription id together with the backing client handle a
// notification must be pushed under.
template <typename Fixture>
struct ContractItem {
  SubscriptionId subscription_id = 0;
  std::size_t backing_index = 0;
  UInt32 backing_client_handle = 0;
};

template <typename Fixture>
ContractItem<Fixture> CreateSubscriptionWithItem(
    Fixture& fixture,
    typename Fixture::ConnectionState& connection,
    UInt32 client_handle,
    NumericId node_id) {
  const auto subscription =
      fixture.template HandleResponse<CreateSubscriptionResponse>(
          connection, CreateSubscriptionRequest{
                          .parameters = {.publishing_interval_ms = 100,
                                         .lifetime_count = 60,
                                         .max_keep_alive_count = 3,
                                         .publishing_enabled = true}});
  EXPECT_EQ(subscription.status.code(), StatusCode::Good);

  const auto created_items =
      fixture.template HandleResponse<CreateMonitoredItemsResponse>(
          connection,
          CreateMonitoredItemsRequest{
              .subscription_id = subscription.subscription_id,
              .items_to_create = {
                  {.item_to_monitor = {.node_id = NumericNode(node_id),
                                       .attribute_id = AttributeId::Value},
                   .requested_parameters = {.client_handle = client_handle,
                                            .queue_size = 1,
                                            .discard_oldest = true}}}});
  EXPECT_EQ(created_items.status.code(), StatusCode::Good);
  fixture.Drain();

  const std::size_t backing_index = fixture.backing_states_->size() - 1;
  EXPECT_EQ(fixture.backing(backing_index).added_items.size(), 1u);
  return {.subscription_id = subscription.subscription_id,
          .backing_index = backing_index,
          .backing_client_handle =
              fixture.backing(backing_index).BackingClientHandle(0)};
}

template <typename Fixture>
void PushDataChange(Fixture& fixture,
                    const ContractItem<Fixture>& item,
                    double value) {
  fixture.backing(item.backing_index)
      .PushDataChange(
          item.backing_client_handle,
          DataValue{Variant{value}, {}, fixture.now_, fixture.now_});
  fixture.Drain();
}

// --- the contract ---------------------------------------------------------

template <typename Fixture>
void ExpectRoutesReadRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  fixture.services_.read =
      [&](ServiceContext context,
          std::vector<ReadValueId>) -> StatusOr<std::vector<DataValue>> {
    EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
    return std::vector{
        DataValue{LocalizedText{u"Pump"}, {}, fixture.now_, fixture.now_}};
  };

  const auto response = fixture.template HandleResponse<ua::ReadResponse>(
      connection,
      ua::ReadRequest{.nodes_to_read = {{.node_id = NumericNode(1),
                                         .attribute_id = static_cast<UInt32>(
                                             AttributeId::DisplayName)}}});

  EXPECT_EQ(response.response_header.service_result.code(), StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].value, Variant{LocalizedText{u"Pump"}});

  // The runtime converts the generated ua::ReadValueId to the hand-written
  // ReadValueId the callback speaks, and carries the session's identity.
  ASSERT_EQ(fixture.services_.last_read_inputs.size(), 1u);
  EXPECT_EQ(fixture.services_.last_read_inputs[0].node_id, NumericNode(1));
  EXPECT_EQ(fixture.services_.last_read_inputs[0].attribute_id,
            AttributeId::DisplayName);
  EXPECT_EQ(fixture.services_.last_read_context.user_id(),
            fixture.expected_user_id_);
}

template <typename Fixture>
void ExpectRoutesWriteRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto response = fixture.template HandleResponse<ua::WriteResponse>(
      connection,
      ua::WriteRequest{
          .nodes_to_write = {
              {.node_id = NumericNode(2),
               .attribute_id = static_cast<UInt32>(AttributeId::Value),
               .value = DataValue{Variant{7.5}, {}, DateTime{}, DateTime{}}}}});

  EXPECT_EQ(response.response_header.service_result.code(), StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].code(), StatusCode::Good);

  ASSERT_EQ(fixture.services_.last_write_inputs.size(), 1u);
  EXPECT_EQ(fixture.services_.last_write_inputs[0].node_id, NumericNode(2));
  EXPECT_EQ(fixture.services_.last_write_context.user_id(),
            fixture.expected_user_id_);
}

template <typename Fixture>
void ExpectRoutesCallRequestsThroughActivatedSessionUser(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  fixture.services_.call = [&](NodeId, NodeId, std::vector<Variant> arguments,
                               ServiceContext context) -> StatusOr<CallResult> {
    EXPECT_EQ(context.user_id(), fixture.expected_user_id_);
    return CallResult{.output_arguments = std::move(arguments)};
  };

  const auto response = fixture.template HandleResponse<ua::CallResponse>(
      connection, ua::CallRequest{.methods_to_call = {
                                      {.object_id = NumericNode(3),
                                       .method_id = NumericNode(4),
                                       .input_arguments = {Variant{11.0}}}}});

  EXPECT_EQ(response.response_header.service_result.code(), StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  ASSERT_EQ(response.results[0].output_arguments.size(), 1u);
  EXPECT_EQ(response.results[0].output_arguments[0], Variant{11.0});
  EXPECT_EQ(fixture.services_.last_call_object_id, NumericNode(3));
  EXPECT_EQ(fixture.services_.last_call_method_id, NumericNode(4));
  EXPECT_EQ(fixture.services_.last_call_context.user_id(),
            fixture.expected_user_id_);
}

template <typename Fixture>
void ExpectServiceRequestsWithoutActivatedSessionAreRejected(Fixture& fixture) {
  typename Fixture::ConnectionState connection;

  // No CreateSession/ActivateSession on this connection. The runtime answers
  // a ServiceFault rather than a service response carrying a bad header — the
  // request never reaches the service layer at all. OPC UA Part 4 §5.6.3
  // ActivateSession,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.6.3
  const auto fault = fixture.template HandleResponse<ServiceFault>(
      connection,
      ua::ReadRequest{.nodes_to_read = {{.node_id = NumericNode(1),
                                         .attribute_id = static_cast<UInt32>(
                                             AttributeId::Value)}}});

  EXPECT_EQ(fault.status.code(), StatusCode::Bad_SessionIdInvalid);
  EXPECT_EQ(fixture.services_.read_count, 0);
}

template <typename Fixture>
void ExpectHistoryReadRawPreservesPayloadThroughActivatedSession(
    Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto from = fixture.now_;
  const auto to = fixture.now_ + Duration::FromSeconds(60);
  fixture.services_.history_read_raw =
      [&](HistoryReadRawDetails details) -> StatusOr<HistoryReadRawResult> {
    EXPECT_EQ(details.node_id, NumericNode(9));
    return HistoryReadRawResult{
        .values = {DataValue{Variant{1.5}, {}, from, from},
                   DataValue{Variant{2.5}, {}, to, to}}};
  };

  const auto response =
      fixture.template HandleResponse<ua::HistoryReadResponse>(
          connection, ua::HistoryReadRequest{
                          .history_read_details =
                              ua::ToExtensionObject(ua::ReadRawModifiedDetails{
                                  .start_time = from, .end_time = to}),
                          .nodes_to_read = {{.node_id = NumericNode(9)}}});

  EXPECT_EQ(response.response_header.service_result.code(), StatusCode::Good);
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].status_code.code(), StatusCode::Good);
  EXPECT_EQ(fixture.services_.history_read_raw_count, 1);
  ASSERT_TRUE(fixture.services_.last_history_read_raw.has_value());
  EXPECT_EQ(fixture.services_.last_history_read_raw->node_id, NumericNode(9));
}

template <typename Fixture>
void ExpectRejectsHistoryReadRawWithoutActivatedSession(Fixture& fixture) {
  typename Fixture::ConnectionState connection;

  const auto fault = fixture.template HandleResponse<ServiceFault>(
      connection, ua::HistoryReadRequest{
                      .history_read_details = ua::ToExtensionObject(
                          ua::ReadRawModifiedDetails{.start_time = fixture.now_,
                                                     .end_time = fixture.now_}),
                      .nodes_to_read = {{.node_id = NumericNode(9)}}});

  EXPECT_EQ(fault.status.code(), StatusCode::Bad_SessionIdInvalid);
  EXPECT_EQ(fixture.services_.history_read_raw_count, 0);
}

template <typename Fixture>
void ExpectNodeManagementMutationsPreserveBatchResults(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  fixture.services_.add_nodes = [](ServiceContext,
                                   std::vector<AddNodesItem> items)
      -> StatusOr<std::vector<AddNodesResult>> {
    // Second item fails: a batch answers per operation, not all-or-nothing.
    std::vector<AddNodesResult> results;
    for (std::size_t i = 0; i < items.size(); ++i) {
      results.push_back({.status_code = i == 1 ? StatusCode::Bad_NodeIdExists
                                               : StatusCode::Good,
                         .added_node_id = items[i].requested_id});
    }
    return results;
  };

  const auto response = fixture.template HandleResponse<ua::AddNodesResponse>(
      connection,
      ua::AddNodesRequest{
          .nodes_to_add = {
              {.requested_new_node_id = ExpandedNodeId{NumericNode(31)}},
              {.requested_new_node_id = ExpandedNodeId{NumericNode(32)}}}});

  EXPECT_EQ(response.response_header.service_result.code(), StatusCode::Good);
  ASSERT_EQ(response.results.size(), 2u);
  EXPECT_EQ(response.results[0].status_code.code(), StatusCode::Good);
  EXPECT_EQ(response.results[1].status_code.code(),
            StatusCode::Bad_NodeIdExists);
  EXPECT_EQ(fixture.services_.last_add_nodes_items.size(), 2u);
}

template <typename Fixture>
void ExpectPreservesLiveSubscriptionStateAcrossDetachAndResume(
    Fixture& fixture) {
  typename Fixture::ConnectionState first_connection;
  const auto ids = fixture.CreateAndActivate(first_connection);

  const auto item = CreateSubscriptionWithItem(fixture, first_connection,
                                               /*client_handle=*/44,
                                               /*node_id=*/11);
  PushDataChange(fixture, item, 12.5);

  fixture.Detach(first_connection);
  EXPECT_FALSE(first_connection.authentication_token.has_value());

  // The session resumes on a second connection, and the subscription it owns
  // is still live with its queued notification.
  typename Fixture::ConnectionState second_connection;
  const auto resumed = fixture.template HandleResponse<ActivateSessionResponse>(
      second_connection, ActivateSessionRequest{
                             .session_id = ids.session_id,
                             .authentication_token = ids.authentication_token,
                         });
  EXPECT_EQ(resumed.status.code(), StatusCode::Good);
  EXPECT_TRUE(resumed.resumed);

  fixture.Advance(100);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      second_connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, item.subscription_id);
  const auto data_change = SingleDataChange(publish);
  ASSERT_TRUE(data_change.has_value());
  EXPECT_EQ(data_change->client_handle, 44u);
  EXPECT_EQ(data_change->value, 12.5);
}

template <typename Fixture>
void ExpectTransfersSubscriptionsAcrossSessions(Fixture& fixture) {
  typename Fixture::ConnectionState source_connection;
  fixture.CreateAndActivate(source_connection);

  const auto item = CreateSubscriptionWithItem(fixture, source_connection,
                                               /*client_handle=*/55,
                                               /*node_id=*/21);
  PushDataChange(fixture, item, 33.5);

  typename Fixture::ConnectionState target_connection;
  fixture.CreateAndActivate(target_connection);

  const auto transferred =
      fixture.template HandleResponse<ua::TransferSubscriptionsResponse>(
          target_connection, ua::TransferSubscriptionsRequest{
                                 .subscription_ids = {item.subscription_id},
                                 .send_initial_values = true});
  ASSERT_EQ(transferred.results.size(), 1u);
  EXPECT_TRUE(transferred.results[0].status_code.good());

  fixture.Advance(100);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      target_connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, item.subscription_id);
  const auto data_change = SingleDataChange(publish);
  ASSERT_TRUE(data_change.has_value());
  EXPECT_EQ(data_change->value, 33.5);
}

template <typename Fixture>
void ExpectCloseSessionClearsAttachedState(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  const auto ids = fixture.CreateAndActivate(connection);

  const auto closed = fixture.template HandleResponse<CloseSessionResponse>(
      connection,
      CloseSessionRequest{.session_id = ids.session_id,
                          .authentication_token = ids.authentication_token});
  EXPECT_EQ(closed.status.code(), StatusCode::Good);

  // The connection no longer carries a session, so a service request on it is
  // faulted rather than served from stale state.
  const auto fault = fixture.template HandleResponse<ServiceFault>(
      connection,
      ua::ReadRequest{.nodes_to_read = {{.node_id = NumericNode(1),
                                         .attribute_id = static_cast<UInt32>(
                                             AttributeId::Value)}}});
  EXPECT_EQ(fault.status.code(), StatusCode::Bad_SessionIdInvalid);
  EXPECT_EQ(fixture.services_.read_count, 0);
}

template <typename Fixture>
void ExpectPublishReturnsKeepAliveWhenNoNotifications(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto subscription =
      fixture.template HandleResponse<CreateSubscriptionResponse>(
          connection, CreateSubscriptionRequest{
                          .parameters = {.publishing_interval_ms = 100,
                                         .lifetime_count = 60,
                                         .max_keep_alive_count = 3,
                                         .publishing_enabled = true}});
  ASSERT_EQ(subscription.status.code(), StatusCode::Good);

  fixture.Advance(100);
  const auto publish = fixture.template HandleResponse<PublishResponse>(
      connection, PublishRequest{});
  EXPECT_EQ(publish.status.code(), StatusCode::Good);
  EXPECT_EQ(publish.subscription_id, subscription.subscription_id);
  // A keep-alive carries no notifications. OPC UA Part 4 §5.13.5 Publish,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  EXPECT_TRUE(publish.notification_message.notification_data.empty());
}

template <typename Fixture>
void ExpectRepublishReplaysNotificationUntilAcknowledged(Fixture& fixture) {
  typename Fixture::ConnectionState connection;
  fixture.CreateAndActivate(connection);

  const auto item = CreateSubscriptionWithItem(fixture, connection,
                                               /*client_handle=*/66,
                                               /*node_id=*/31);
  PushDataChange(fixture, item, 5.5);
  fixture.Advance(100);

  const auto publish = fixture.template HandleResponse<PublishResponse>(
      connection, PublishRequest{});
  ASSERT_EQ(publish.status.code(), StatusCode::Good);
  const auto sequence_number = publish.notification_message.sequence_number;

  // Unacknowledged, so it replays.
  const auto republished = fixture.template HandleResponse<RepublishResponse>(
      connection,
      RepublishRequest{.subscription_id = item.subscription_id,
                       .retransmit_sequence_number = sequence_number});
  EXPECT_EQ(republished.status.code(), StatusCode::Good);
  EXPECT_EQ(republished.notification_message.sequence_number, sequence_number);

  // Acknowledging it (on the next Publish) releases it.
  fixture.Advance(100);
  fixture.template HandleResponse<PublishResponse>(
      connection, PublishRequest{.subscription_acknowledgements = {
                                     {.subscription_id = item.subscription_id,
                                      .sequence_number = sequence_number}}});

  const auto after_ack = fixture.template HandleResponse<RepublishResponse>(
      connection,
      RepublishRequest{.subscription_id = item.subscription_id,
                       .retransmit_sequence_number = sequence_number});
  EXPECT_EQ(after_ack.status.code(), StatusCode::Bad_MessageNotAvailable);
}

}  // namespace opcua::test
