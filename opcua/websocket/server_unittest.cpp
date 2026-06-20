#include "opcua/websocket/server.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/server_runtime_contract_test.h"
#include "opcua/websocket/json_codec.h"
#include "opcua/scada/attribute_service_mock.h"
#include "opcua/scada/authentication_adapters.h"
#include "opcua/scada/history_service_mock.h"
#include "opcua/scada/method_service_mock.h"
#include "opcua/scada/node_management_service_mock.h"
#include "opcua/scada/test/test_monitored_item.h"
#include "opcua/scada/view_service_mock.h"
#include "transport/transport.h"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <deque>

using namespace testing;

namespace opcua::ws {
namespace {

opcua::scada::NodeId NumericNode(opcua::scada::NumericId id, opcua::scada::NamespaceIndex ns = 2) {
  return {id, ns};
}

struct MessagePeerState {
  std::deque<std::string> incoming;
  std::vector<std::string> writes;
  bool opened = false;
  bool closed = false;
};

class ScriptedMessageTransport {
 public:
  ScriptedMessageTransport(transport::executor executor,
                           std::shared_ptr<MessagePeerState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedMessageTransport(ScriptedMessageTransport&&) = default;
  ScriptedMessageTransport& operator=(ScriptedMessageTransport&&) = default;
  ScriptedMessageTransport(const ScriptedMessageTransport&) = delete;
  ScriptedMessageTransport& operator=(const ScriptedMessageTransport&) = delete;

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }

  transport::awaitable<transport::expected<size_t>> read(std::span<char> data) {
    if (state_->incoming.empty())
      co_return size_t{0};

    auto message = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    if (message.size() > data.size())
      co_return transport::ERR_INVALID_ARGUMENT;

    std::ranges::copy(message, data.begin());
    co_return message.size();
  }

  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    state_->writes.emplace_back(data.begin(), data.end());
    co_return data.size();
  }

  std::string name() const { return "ScriptedMessageTransport"; }
  bool message_oriented() const { return true; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<MessagePeerState> state_;
};

struct AcceptorState {
  std::deque<transport::any_transport> accepted;
  bool opened = false;
  bool closed = false;
};

class ScriptedAcceptorTransport {
 public:
  ScriptedAcceptorTransport(transport::executor executor,
                            std::shared_ptr<AcceptorState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedAcceptorTransport(ScriptedAcceptorTransport&&) = default;
  ScriptedAcceptorTransport& operator=(ScriptedAcceptorTransport&&) = default;
  ScriptedAcceptorTransport(const ScriptedAcceptorTransport&) = delete;
  ScriptedAcceptorTransport& operator=(const ScriptedAcceptorTransport&) =
      delete;

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    if (state_->accepted.empty())
      co_return transport::ERR_ABORTED;
    auto next = std::move(state_->accepted.front());
    state_->accepted.pop_front();
    co_return std::move(next);
  }

  transport::awaitable<transport::expected<size_t>> read(std::span<char>) {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }

  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char>) {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }

  std::string name() const { return "ScriptedAcceptorTransport"; }
  bool message_oriented() const { return true; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return false; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<AcceptorState> state_;
};

class TestMonitoredItemService : public opcua::scada::MonitoredItemService {
 public:
  std::shared_ptr<opcua::scada::MonitoredItem> CreateMonitoredItem(
      const opcua::scada::ReadValueId&,
      const opcua::scada::MonitoringParameters&) {
    return std::make_shared<opcua::scada::TestMonitoredItem>();
  }

  opcua::scada::StatusOr<std::unique_ptr<opcua::scada::MonitoredItemSubscription>>
  CreateSubscription(opcua::scada::ServiceContext /*context*/,
                     opcua::scada::MonitoredItemSubscriptionOptions options) override {
    return opcua::scada::MakeItemFactorySubscription(
        [this](const opcua::scada::ReadValueId& value_id,
               const opcua::scada::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }
};

class ServerTest : public Test {
 protected:
  std::string Encode(const RequestMessage& request) {
    return boost::json::serialize(EncodeJson(request));
  }

  ResponseMessage DecodeResponse(const std::string& response) {
    return *DecodeResponseMessage(boost::json::parse(response));
  }

  void ServePeer(const std::shared_ptr<MessagePeerState>& peer) {
    opcua::WaitAwaitable(executor_, server_->ServeConnection(MakePeer(peer)));
  }

  transport::any_transport MakePeer(
      const std::shared_ptr<MessagePeerState>& peer) {
    return transport::any_transport{
        ScriptedMessageTransport{any_executor_, peer}};
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  StrictMock<opcua::scada::MockAttributeService> attribute_service_;
  StrictMock<opcua::scada::MockViewService> view_service_;
  StrictMock<opcua::scada::MockHistoryService> history_service_;
  StrictMock<opcua::scada::MockMethodService> method_service_;
  StrictMock<opcua::scada::MockNodeManagementService> node_management_service_;
  TestMonitoredItemService monitored_item_service_;
  ServerSessionManager session_manager_{{
      .authenticator = opcua::scada::MakeCoroutineAuthenticator(
          [](opcua::scada::LocalizedText, opcua::scada::LocalizedText)
              -> opcua::Awaitable<opcua::scada::StatusOr<opcua::scada::AuthenticationResult>> {
            co_return opcua::scada::AuthenticationResult{
                .user_id = opcua::scada::NodeId{55, 3}, .multi_sessions = true};
          }),
  }};
  ServerRuntime runtime_{{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = attribute_service_,
      .view_service = view_service_,
      .history_service = history_service_,
      .method_service = method_service_,
      .node_management_service = node_management_service_,
  }};
  std::shared_ptr<AcceptorState> acceptor_state_ =
      std::make_shared<AcceptorState>();
  std::unique_ptr<Server> server_ = std::make_unique<Server>(ServerContext{
      .acceptor = transport::any_transport{ScriptedAcceptorTransport{
          any_executor_, acceptor_state_}},
      .runtime = runtime_,
      .max_message_size = 1024,
  });
};

TEST_F(ServerTest, ServeConnectionProcessesRequestFramesEndToEnd) {
  auto peer = std::make_shared<MessagePeerState>();
  peer->incoming.push_back(
      Encode({.request_handle = 1, .body = CreateSessionRequest{}}));
  peer->incoming.push_back(
      Encode({.request_handle = 2,
              .body = ActivateSessionRequest{
                  .session_id = NumericNode(1),
                  .authentication_token = NumericNode(1, 3),
                  .user_name = opcua::scada::LocalizedText{u"operator"},
                  .password = opcua::scada::LocalizedText{u"secret"}}}));
  peer->incoming.push_back(Encode(
      {.request_handle = 3,
       .body = ReadRequest{
           .inputs = {{.node_id = NumericNode(7),
                       .attribute_id = opcua::scada::AttributeId::DisplayName}}}}));
  EXPECT_CALL(attribute_service_, Read(_, _))
      .WillOnce(Invoke(
          [&](opcua::scada::ServiceContext context,
              std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs)
              -> opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> {
            EXPECT_EQ(context.user_id(), (opcua::scada::NodeId{55, 3}));
            EXPECT_THAT(*inputs,
                        ElementsAre(opcua::scada::ReadValueId{
                            .node_id = NumericNode(7),
                            .attribute_id = opcua::scada::AttributeId::DisplayName}));
            co_return std::vector{
                opcua::scada::DataValue{opcua::scada::LocalizedText{u"Pump"},
                                 {},
                                 opcua::base::Time::Now(),
                                 opcua::base::Time::Now()}};
          }));

  ServePeer(peer);

  ASSERT_EQ(peer->writes.size(), 3u);
  const auto create_response =
      std::get<CreateSessionResponse>(DecodeResponse(peer->writes[0]).body);
  EXPECT_EQ(create_response.status.code(), opcua::scada::StatusCode::Good);

  const auto activate_response =
      std::get<ActivateSessionResponse>(DecodeResponse(peer->writes[1]).body);
  EXPECT_EQ(activate_response.status.code(), opcua::scada::StatusCode::Good);

  const auto read_response =
      std::get<ReadResponse>(DecodeResponse(peer->writes[2]).body);
  EXPECT_EQ(read_response.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(read_response.results.size(), 1u);
  EXPECT_EQ(read_response.results[0].value,
            opcua::scada::Variant{opcua::scada::LocalizedText{u"Pump"}});
}

TEST_F(ServerTest, InvalidJsonProducesServiceFault) {
  auto peer = std::make_shared<MessagePeerState>();
  peer->incoming.push_back("{not-json");
  ServePeer(peer);

  ASSERT_EQ(peer->writes.size(), 1u);
  const auto response = DecodeResponse(peer->writes[0]);
  const auto* fault = std::get_if<ServiceFault>(&response.body);
  ASSERT_NE(fault, nullptr);
  EXPECT_EQ(fault->status.code(), opcua::scada::StatusCode::Bad_CantParseString);
}

TEST_F(ServerTest, DisconnectDetachesSessionForResume) {
  auto first_peer = std::make_shared<MessagePeerState>();
  first_peer->incoming.push_back(
      Encode({.request_handle = 1, .body = CreateSessionRequest{}}));
  first_peer->incoming.push_back(
      Encode({.request_handle = 2,
              .body = ActivateSessionRequest{
                  .session_id = NumericNode(1),
                  .authentication_token = NumericNode(1, 3),
                  .user_name = opcua::scada::LocalizedText{u"operator"},
                  .password = opcua::scada::LocalizedText{u"secret"}}}));

  ServePeer(first_peer);

  auto second_peer = std::make_shared<MessagePeerState>();
  second_peer->incoming.push_back(
      Encode({.request_handle = 3,
              .body = ActivateSessionRequest{
                  .session_id = NumericNode(1),
                  .authentication_token = NumericNode(1, 3)}}));
  ServePeer(second_peer);

  ASSERT_EQ(second_peer->writes.size(), 1u);
  const auto resumed = std::get<ActivateSessionResponse>(
      DecodeResponse(second_peer->writes[0]).body);
  EXPECT_EQ(resumed.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_TRUE(resumed.resumed);
}

TEST_F(ServerTest, ServeConnectionRoutesReadThroughCoroutineServices) {
  test::TestCoroutineServices coroutine_services;
  ServerRuntime coroutine_runtime{ServerRuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = coroutine_services,
      .view_service = coroutine_services,
      .history_service = coroutine_services,
      .method_service = coroutine_services,
      .node_management_service = coroutine_services,
  }};
  Server coroutine_server{ServerContext{
      .acceptor = transport::any_transport{ScriptedAcceptorTransport{
          any_executor_, std::make_shared<AcceptorState>()}},
      .runtime = coroutine_runtime,
      .max_message_size = 1024,
  }};

  auto peer = std::make_shared<MessagePeerState>();
  peer->incoming.push_back(
      Encode({.request_handle = 1, .body = CreateSessionRequest{}}));
  peer->incoming.push_back(
      Encode({.request_handle = 2,
              .body = ActivateSessionRequest{
                  .session_id = NumericNode(1),
                  .authentication_token = NumericNode(1, 3),
                  .user_name = opcua::scada::LocalizedText{u"operator"},
                  .password = opcua::scada::LocalizedText{u"secret"}}}));
  const ReadRequest read_request{
      .inputs = {{.node_id = NumericNode(902),
                  .attribute_id = opcua::scada::AttributeId::Value}}};
  peer->incoming.push_back(Encode({.request_handle = 3, .body = read_request}));

  opcua::WaitAwaitable(executor_, coroutine_server.ServeConnection(MakePeer(peer)));

  ASSERT_EQ(peer->writes.size(), 3u);
  const auto read_response =
      std::get<ReadResponse>(DecodeResponse(peer->writes[2]).body);
  EXPECT_EQ(read_response.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(read_response.results.size(), 1u);
  EXPECT_EQ(read_response.results[0].value, coroutine_services.read_value);
  EXPECT_EQ(coroutine_services.read_count, 1);
  EXPECT_EQ(coroutine_services.last_read_context.user_id(),
            (opcua::scada::NodeId{55, 3}));
  EXPECT_THAT(coroutine_services.last_read_inputs,
              ElementsAre(read_request.inputs[0]));
}

TEST_F(ServerTest, OpenAndCloseDriveAcceptorLifecycle) {
  EXPECT_FALSE(acceptor_state_->opened);
  EXPECT_FALSE(acceptor_state_->closed);

  EXPECT_EQ(opcua::WaitAwaitable(executor_, server_->Open()), transport::OK);
  Drain(executor_);
  EXPECT_TRUE(acceptor_state_->opened);

  EXPECT_EQ(opcua::WaitAwaitable(executor_, server_->Close()), transport::OK);
  EXPECT_TRUE(acceptor_state_->closed);
}

}  // namespace
}  // namespace opcua::ws
