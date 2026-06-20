#include "opcua/binary/client_secure_channel.h"
#include "opcua/binary/client_transport.h"
#include "opcua/binary/protocol.h"
#include "opcua/binary/runtime.h"
#include "opcua/binary/secure_channel.h"
#include "opcua/binary/service_codec.h"
#include "opcua/binary/service_dispatcher.h"
#include "opcua/server_session_manager.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/scada/attribute_service.h"
#include "opcua/scada/authentication_adapters.h"
#include "opcua/scada/history_service.h"
#include "opcua/scada/method_service.h"
#include "opcua/scada/monitored_item_service.h"
#include "opcua/scada/node_management_service.h"
#include "opcua/scada/view_service.h"
#include "transport/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// End-to-end interoperability smoke test: the in-repo OPC UA binary *client*
// (ClientSecureChannel + the public service codec) talks to the real server
// stack (SecureChannel -> ServiceDispatcher -> Runtime -> ServerSessionManager
// + data services) over a loopback transport, exercising the full
// HEL/ACK -> OpenSecureChannel -> CreateSession -> ActivateSession ->
// Read/Browse -> CloseSession lifecycle. This is the automatable row of the
// interoperability matrix (server/docs/opcua_interop_matrix.md); external
// clients are exercised manually per that document.
namespace opcua::binary {
namespace {

// ---- Minimal data-service fakes ------------------------------------------

class FakeAttributeService : public opcua::scada::AttributeService {
 public:
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::DataValue>>> Read(
      opcua::scada::ServiceContext,
      std::shared_ptr<const std::vector<opcua::scada::ReadValueId>> inputs) override {
    std::vector<opcua::scada::DataValue> results(
        inputs->size(), opcua::scada::MakeReadResult(opcua::scada::Int32{42}));
    co_return results;
  }
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::StatusCode>>> Write(
      opcua::scada::ServiceContext,
      std::shared_ptr<const std::vector<opcua::scada::WriteValue>> inputs) override {
    co_return std::vector<opcua::scada::StatusCode>(inputs->size(),
                                             opcua::scada::StatusCode::Good);
  }
};

class FakeViewService : public opcua::scada::ViewService {
 public:
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::BrowseResult>>> Browse(
      opcua::scada::ServiceContext,
      std::vector<opcua::scada::BrowseDescription> inputs) override {
    co_return std::vector<opcua::scada::BrowseResult>(inputs.size());
  }
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::BrowsePathResult>>>
  TranslateBrowsePaths(std::vector<opcua::scada::BrowsePath> inputs) override {
    co_return std::vector<opcua::scada::BrowsePathResult>(inputs.size());
  }
};

class FakeHistoryService : public opcua::scada::HistoryService {
 public:
  opcua::Awaitable<opcua::scada::HistoryReadRawResult> HistoryReadRaw(
      opcua::scada::HistoryReadRawDetails) override {
    co_return opcua::scada::HistoryReadRawResult{};
  }
  opcua::Awaitable<opcua::scada::HistoryReadEventsResult> HistoryReadEvents(
      opcua::scada::NodeId,
      opcua::scada::DateTime,
      opcua::scada::DateTime,
      opcua::scada::EventFilter) override {
    co_return opcua::scada::HistoryReadEventsResult{};
  }
};

class FakeMethodService : public opcua::scada::MethodService {
 public:
  opcua::Awaitable<opcua::scada::Status> Call(opcua::scada::NodeId,
                                opcua::scada::NodeId,
                                std::vector<opcua::scada::Variant>,
                                opcua::scada::NodeId) override {
    co_return opcua::scada::Status{opcua::scada::StatusCode::Bad_WrongMethodId};
  }
};

class FakeNodeManagementService : public opcua::scada::NodeManagementService {
 public:
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::AddNodesResult>>> AddNodes(
      std::vector<opcua::scada::AddNodesItem> inputs) override {
    co_return std::vector<opcua::scada::AddNodesResult>(inputs.size());
  }
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::StatusCode>>> DeleteNodes(
      std::vector<opcua::scada::DeleteNodesItem> inputs) override {
    co_return std::vector<opcua::scada::StatusCode>(inputs.size(),
                                             opcua::scada::StatusCode::Good);
  }
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::StatusCode>>> AddReferences(
      std::vector<opcua::scada::AddReferencesItem> inputs) override {
    co_return std::vector<opcua::scada::StatusCode>(inputs.size(),
                                             opcua::scada::StatusCode::Good);
  }
  opcua::Awaitable<opcua::scada::StatusOr<std::vector<opcua::scada::StatusCode>>> DeleteReferences(
      std::vector<opcua::scada::DeleteReferencesItem> inputs) override {
    co_return std::vector<opcua::scada::StatusCode>(inputs.size(),
                                             opcua::scada::StatusCode::Good);
  }
};

class FakeMonitoredItemService : public opcua::scada::MonitoredItemService {
 public:
  opcua::scada::StatusOr<std::unique_ptr<opcua::scada::MonitoredItemSubscription>>
  CreateSubscription(opcua::scada::ServiceContext,
                     opcua::scada::MonitoredItemSubscriptionOptions) override {
    return opcua::scada::Status{opcua::scada::StatusCode::Bad};
  }
};

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

// Loopback transport that routes every client frame through the real server
// stack: HEL is answered with ACK; SecureChannel frames go to the server
// SecureChannel, and decrypted service payloads are dispatched through the
// ServiceDispatcher (Runtime + session manager + data services), with the
// response framed back to the client.
struct LoopbackState {
  SecureChannel* server = nullptr;
  ServiceDispatcher* dispatcher = nullptr;
  ConnectionState* connection = nullptr;
  std::deque<std::string> incoming;
  bool opened = false;
  bool closed = false;
};

class LoopbackTransport {
 public:
  LoopbackTransport(transport::executor executor,
                    std::shared_ptr<LoopbackState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  LoopbackTransport(LoopbackTransport&&) = default;
  LoopbackTransport& operator=(LoopbackTransport&&) = default;
  LoopbackTransport(const LoopbackTransport&) = delete;
  LoopbackTransport& operator=(const LoopbackTransport&) = delete;

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
    if (state_->incoming.empty()) {
      co_return size_t{0};
    }
    auto chunk = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    if (chunk.size() > data.size()) {
      co_return transport::ERR_INVALID_ARGUMENT;
    }
    std::ranges::copy(chunk, data.begin());
    co_return chunk.size();
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    const std::vector<char> frame{data.begin(), data.end()};
    if (frame.size() >= 3 && frame[0] == 'H' && frame[1] == 'E' &&
        frame[2] == 'L') {
      state_->incoming.push_back(AsString(EncodeAcknowledgeMessage(
          {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
      co_return data.size();
    }

    auto result = co_await state_->server->HandleFrame(frame);
    if (result.outbound_frame.has_value() && !result.outbound_frame->empty()) {
      state_->incoming.push_back(AsString(*result.outbound_frame));
    }
    if (result.service_payload.has_value() && result.request_id.has_value()) {
      state_->connection->secure_channel = state_->server->secure();
      state_->connection->client_certificate =
          state_->server->client_certificate();
      auto response_body =
          co_await state_->dispatcher->HandlePayload(*result.service_payload);
      if (response_body.has_value() && !response_body->empty()) {
        auto frame_out = state_->server->BuildServiceResponse(
            *result.request_id, std::move(*response_body));
        if (!frame_out.empty()) {
          state_->incoming.push_back(AsString(frame_out));
        }
      }
    }
    if (result.close_transport) {
      state_->closed = true;
    }
    co_return data.size();
  }
  std::string name() const { return "LoopbackTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<LoopbackState> state_;
};

class ClientServerE2ETest : public ::testing::Test {
 protected:
  // Sends one service request through the client secure channel and decodes the
  // typed response.
  template <typename ResponseT>
  ResponseT Call(ClientSecureChannel& client,
                 const opcua::scada::NodeId& authentication_token,
                 RequestBody body) {
    const ServiceRequestHeader header{
        .authentication_token = authentication_token,
        .request_handle = ++request_handle_};
    auto encoded = EncodeServiceRequest(header, body);
    EXPECT_TRUE(encoded.has_value());
    const auto request_id = client.NextRequestId();
    EXPECT_TRUE(
        opcua::WaitAwaitable(executor_, client.SendServiceRequest(request_id, *encoded))
            .good());
    auto response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
    EXPECT_TRUE(response.ok());
    auto decoded = DecodeServiceResponse(response->body);
    EXPECT_TRUE(decoded.has_value());
    return std::get<ResponseT>(decoded->body);
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  std::uint32_t request_handle_ = 0;

  FakeAttributeService attribute_service_;
  FakeViewService view_service_;
  FakeHistoryService history_service_;
  FakeMethodService method_service_;
  FakeNodeManagementService node_management_service_;
  FakeMonitoredItemService monitored_item_service_;
  ServerSessionManager session_manager_{{
      .authenticator = opcua::scada::MakeCoroutineAuthenticator(
          [](opcua::scada::LocalizedText, opcua::scada::LocalizedText)
              -> opcua::Awaitable<opcua::scada::StatusOr<opcua::scada::AuthenticationResult>> {
            co_return opcua::scada::AuthenticationResult{.user_id = opcua::scada::NodeId{1, 0},
                                                  .multi_sessions = true};
          }),
  }};
  ConnectionState connection_;
  Runtime runtime_{binary::RuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = attribute_service_,
      .view_service = view_service_,
      .history_service = history_service_,
      .method_service = method_service_,
      .node_management_service = node_management_service_,
  }};
};

TEST_F(ClientServerE2ETest, NonePolicySessionReadBrowseLifecycle) {
  SecureChannel server{/*channel_id=*/77};
  ServiceDispatcher dispatcher{{.runtime = runtime_, .connection = connection_}};
  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  state->dispatcher = &dispatcher;
  state->connection = &connection_;

  auto client_transport = std::make_unique<ClientTransport>(ClientTransportContext{
      .transport = transport::any_transport{LoopbackTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  });
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  // CreateSession.
  const auto created = Call<CreateSessionResponse>(client, opcua::scada::NodeId{},
                                                   CreateSessionRequest{});
  ASSERT_EQ(created.status.code(), opcua::scada::StatusCode::Good);
  const auto token = created.authentication_token;
  ASSERT_FALSE(token.is_null());

  // ActivateSession (anonymous).
  const auto activated = Call<ActivateSessionResponse>(
      client, token, ActivateSessionRequest{.allow_anonymous = true});
  ASSERT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);

  // Read.
  const auto read = Call<ReadResponse>(
      client, token,
      ReadRequest{.inputs = {{.node_id = opcua::scada::NodeId{1, 2},
                              .attribute_id = opcua::scada::AttributeId::Value}}});
  ASSERT_EQ(read.status.code(), opcua::scada::StatusCode::Good);
  ASSERT_EQ(read.results.size(), 1u);
  EXPECT_EQ(read.results[0].value, opcua::scada::Variant{opcua::scada::Int32{42}});

  // Browse.
  const auto browse = Call<BrowseResponse>(
      client, token,
      BrowseRequest{.inputs = {{.node_id = opcua::scada::NodeId{1, 2}}}});
  EXPECT_EQ(browse.status.code(), opcua::scada::StatusCode::Good);

  // CloseSession.
  const auto closed =
      Call<CloseSessionResponse>(client, token, CloseSessionRequest{});
  EXPECT_EQ(closed.status.code(), opcua::scada::StatusCode::Good);
}

}  // namespace
}  // namespace opcua::binary
