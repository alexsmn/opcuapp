#include "opcua/transport/websocket/server.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/session/server_runtime_contract_test.h"
#include "opcua/transport/websocket/json_codec.h"
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

opcua::NodeId NumericNode(opcua::NumericId id, opcua::NamespaceIndex ns = 2) {
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
  // The removed service mocks and TestMonitoredItemService are replaced by the
  // shared recording fake and FakeMonitoredItemSubscription; see
  // server_runtime_contract_test.h.
  opcua::test::ScriptedServices services_;
  std::shared_ptr<opcua::test::BackingStates> backing_states_ =
      std::make_shared<opcua::test::BackingStates>();
  ServerSessionManager session_manager_{{
      .authenticator = opcua::MakeCoroutineAuthenticator(
          [](opcua::LocalizedText, opcua::LocalizedText)
              -> opcua::Awaitable<
                  opcua::StatusOr<opcua::AuthenticationResult>> {
            co_return opcua::AuthenticationResult{
                .user_id = opcua::NodeId{55, 3}, .multi_sessions = true};
          }),
  }};
  ServerRuntime runtime_{{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .callbacks = services_.MakeCallbacks(any_executor_, backing_states_),
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
                  .user_name = opcua::LocalizedText{u"operator"},
                  .password = opcua::LocalizedText{u"secret"}}}));
  peer->incoming.push_back(
      Encode({.request_handle = 3,
              .body = ua::ReadRequest{
                  .nodes_to_read = {{.node_id = NumericNode(7),
                                     .attribute_id = static_cast<opcua::UInt32>(
                                         opcua::AttributeId::DisplayName)}}}}));
  services_.read = [](opcua::ServiceContext, std::vector<opcua::ReadValueId>)
      -> opcua::StatusOr<std::vector<opcua::DataValue>> {
    return std::vector{opcua::DataValue{opcua::LocalizedText{u"Pump"},
                                        {},
                                        opcua::DateTime::Now(),
                                        opcua::DateTime::Now()}};
  };

  ServePeer(peer);

  ASSERT_EQ(peer->writes.size(), 3u);
  const auto create_response =
      std::get<CreateSessionResponse>(DecodeResponse(peer->writes[0]).body);
  EXPECT_EQ(create_response.status.code(), opcua::StatusCode::Good);

  const auto activate_response =
      std::get<ActivateSessionResponse>(DecodeResponse(peer->writes[1]).body);
  EXPECT_EQ(activate_response.status.code(), opcua::StatusCode::Good);

  const auto read_response =
      std::get<ua::ReadResponse>(DecodeResponse(peer->writes[2]).body);
  EXPECT_EQ(read_response.response_header.service_result.code(),
            opcua::StatusCode::Good);
  ASSERT_EQ(read_response.results.size(), 1u);
  EXPECT_EQ(read_response.results[0].value,
            opcua::Variant{opcua::LocalizedText{u"Pump"}});

  // Absorbed from the former ServeConnectionRoutesReadThroughCoroutineServices,
  // which existed only to assert what the application actually received. The
  // recording fake this fixture now uses makes that observable here, so the two
  // tests are one.
  EXPECT_EQ(services_.read_count, 1);
  EXPECT_EQ(services_.last_read_context.user_id(), (opcua::NodeId{55, 3}));
  EXPECT_THAT(services_.last_read_inputs,
              ElementsAre(opcua::ReadValueId{
                  .node_id = NumericNode(7),
                  .attribute_id = opcua::AttributeId::DisplayName}));
}

TEST_F(ServerTest, InvalidJsonProducesServiceFault) {
  auto peer = std::make_shared<MessagePeerState>();
  peer->incoming.push_back("{not-json");
  ServePeer(peer);

  ASSERT_EQ(peer->writes.size(), 1u);
  const auto response = DecodeResponse(peer->writes[0]);
  const auto* fault = std::get_if<ServiceFault>(&response.body);
  ASSERT_NE(fault, nullptr);
  EXPECT_EQ(fault->status.code(), opcua::StatusCode::Bad_TypeMismatch);
}

TEST_F(ServerTest, DisconnectDetachesSessionForResume) {
  // The session id and authentication token come from CreateSession rather
  // than being assumed to be {1,ns2}/{1,ns3}: hard-coding the allocator's
  // output made this test assert resume against a session that was never
  // activated, so it reported "not resumed" for the wrong reason.
  auto create_peer = std::make_shared<MessagePeerState>();
  create_peer->incoming.push_back(
      Encode({.request_handle = 1, .body = CreateSessionRequest{}}));
  ServePeer(create_peer);

  ASSERT_EQ(create_peer->writes.size(), 1u);
  const auto created = std::get<CreateSessionResponse>(
      DecodeResponse(create_peer->writes[0]).body);
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);

  auto first_peer = std::make_shared<MessagePeerState>();
  first_peer->incoming.push_back(
      Encode({.request_handle = 2,
              .body = ActivateSessionRequest{
                  .session_id = created.session_id,
                  .authentication_token = created.authentication_token,
                  .user_name = opcua::LocalizedText{u"operator"},
                  .password = opcua::LocalizedText{u"secret"}}}));
  ServePeer(first_peer);

  ASSERT_EQ(first_peer->writes.size(), 1u);
  const auto activated = std::get<ActivateSessionResponse>(
      DecodeResponse(first_peer->writes[0]).body);
  ASSERT_EQ(activated.status.code(), opcua::StatusCode::Good);

  // The first connection is gone; activating the same session on a second one
  // must be accepted rather than rejected as an unknown session — that
  // acceptance is what "detached for resume" means at this boundary.
  auto second_peer = std::make_shared<MessagePeerState>();
  second_peer->incoming.push_back(
      Encode({.request_handle = 3,
              .body = ActivateSessionRequest{
                  .session_id = created.session_id,
                  .authentication_token = created.authentication_token}}));
  ServePeer(second_peer);

  ASSERT_EQ(second_peer->writes.size(), 1u);
  const auto resumed = std::get<ActivateSessionResponse>(
      DecodeResponse(second_peer->writes[0]).body);
  EXPECT_EQ(resumed.status.code(), opcua::StatusCode::Good);

  // KNOWN GAP, pinned deliberately rather than asserted the other way.
  //
  // ServerSessionManager really does resume here — it logs "OPC UA session
  // resumed" and ServerRuntime returns `resumed = true` — but the flag never
  // reaches a client on ANY transport. `resumed` is an opcuapp extension on the
  // hand-written ActivateSessionResponse, and both codecs encode through
  // session_conversion::ToWire into the conformant ua::ActivateSessionResponse
  // (OPC UA Part 4 §5.6.3), which has no such field: response header, server
  // nonce, per-token results, diagnostics. So it is dropped on the way out and
  // decodes back as false.
  //
  // The in-process contract tests (server_runtime_contract_test.h) assert
  // `resumed == true` and pass because they call Handle() directly, with no
  // encoding in between — which is exactly why nothing caught this.
  //
  // Giving the flag a wire home (an additionalHeader extension) or removing it
  // from the public response is a protocol decision, not a test fix, so this
  // pins today's behaviour instead of pretending either way.
  EXPECT_FALSE(resumed.resumed)
      << "the resumed flag now survives encoding — decide its wire contract "
         "and update this test and the in-process contract together";
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
