#include "opcua/session/server_session_manager.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/transport/binary/runtime.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/service_codec.h"
#include "opcua/transport/binary/service_dispatcher.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/services/attribute_types.h"
#include "opcua/services/history_update_types.h"
#include "opcua/services/method_types.h"
#include "opcua/services/node_management_types.h"
#include "opcua/services/service_callbacks.h"
#include "opcua/services/view_types.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/types/co_result.h"
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

// ---- The application behind ServiceCallbacks ------------------------------
//
// The fakes this suite used to declare were implementations of the removed
// AttributeService/ViewService/... interfaces; the runtime now takes plain
// callbacks, so they collapse into this one factory. Behaviour is unchanged:
// Read answers Int32{42} per operation, Browse answers one empty BrowseResult
// per operation, and everything else answers Good.
ServiceCallbacks MakeE2ECallbacks() {
  ServiceCallbacks callbacks;
  callbacks.read =
      [](opcua::ServiceContext,
         std::shared_ptr<const std::vector<opcua::ReadValueId>> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::DataValue>> {
    co_return std::vector<opcua::DataValue>(
        inputs->size(), opcua::MakeReadResult(opcua::Int32{42}));
  };
  callbacks.write =
      [](opcua::ServiceContext,
         std::shared_ptr<const std::vector<opcua::WriteValue>> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::StatusCode>> {
    co_return std::vector<opcua::StatusCode>(inputs->size(),
                                             opcua::StatusCode::Good);
  };
  callbacks.browse = [](opcua::ServiceContext,
                        std::vector<opcua::BrowseDescription> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::BrowseResult>> {
    co_return std::vector<opcua::BrowseResult>(inputs.size());
  };
  callbacks.translate_browse_paths = [](std::vector<opcua::BrowsePath> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::BrowsePathResult>> {
    co_return std::vector<opcua::BrowsePathResult>(inputs.size());
  };
  callbacks.call =
      [](opcua::NodeId, opcua::NodeId, std::vector<opcua::Variant>,
         opcua::ServiceContext) -> opcua::CoStatusOr<opcua::CallResult> {
    co_return opcua::Status{opcua::StatusCode::Bad_MethodInvalid};
  };
  callbacks.history_read_raw = [](opcua::HistoryReadRawDetails)
      -> opcua::CoStatusOr<opcua::HistoryReadRawResult> {
    co_return opcua::HistoryReadRawResult{};
  };
  callbacks.history_read_events = [](opcua::NodeId, opcua::DateTime,
                                     opcua::DateTime, opcua::EventFilter)
      -> opcua::CoStatusOr<opcua::HistoryReadEventsResult> {
    co_return opcua::HistoryReadEventsResult{};
  };
  callbacks.add_nodes = [](opcua::ServiceContext,
                           std::vector<opcua::AddNodesItem> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::AddNodesResult>> {
    co_return std::vector<opcua::AddNodesResult>(inputs.size());
  };
  callbacks.delete_nodes = [](opcua::ServiceContext,
                              std::vector<opcua::DeleteNodesItem> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::StatusCode>> {
    co_return std::vector<opcua::StatusCode>(inputs.size(),
                                             opcua::StatusCode::Good);
  };
  callbacks.add_references = [](opcua::ServiceContext,
                                std::vector<opcua::AddReferencesItem> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::StatusCode>> {
    co_return std::vector<opcua::StatusCode>(inputs.size(),
                                             opcua::StatusCode::Good);
  };
  callbacks.delete_references =
      [](opcua::ServiceContext, std::vector<opcua::DeleteReferencesItem> inputs)
      -> opcua::CoStatusOr<std::vector<opcua::StatusCode>> {
    co_return std::vector<opcua::StatusCode>(inputs.size(),
                                             opcua::StatusCode::Good);
  };
  return callbacks;
}

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
                 const opcua::NodeId& authentication_token,
                 RequestBody body) {
    const ServiceRequestHeader header{
        .authentication_token = authentication_token,
        .request_handle = ++request_handle_};
    auto encoded = EncodeServiceRequest(header, body);
    EXPECT_TRUE(encoded.has_value());
    const auto request_id = client.NextRequestId();
    EXPECT_TRUE(opcua::WaitAwaitable(
                    executor_, client.SendServiceRequest(request_id, *encoded))
                    .good());
    auto response =
        opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
    EXPECT_TRUE(response.ok());
    auto decoded = DecodeServiceResponse(response->body);
    EXPECT_TRUE(decoded.has_value());
    return std::get<ResponseT>(decoded->body);
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  std::uint32_t request_handle_ = 0;

  ServerSessionManager session_manager_{{
      .authenticator = opcua::MakeCoroutineAuthenticator(
          [](opcua::LocalizedText, opcua::LocalizedText)
              -> opcua::Awaitable<
                  opcua::StatusOr<opcua::AuthenticationResult>> {
            co_return opcua::AuthenticationResult{
                .user_id = opcua::NodeId{1, 0}, .multi_sessions = true};
          }),
  }};
  ConnectionState connection_;
  Runtime runtime_{binary::RuntimeContext{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .callbacks = MakeE2ECallbacks(),
  }};
};

TEST_F(ClientServerE2ETest, NonePolicySessionReadBrowseLifecycle) {
  SecureChannel server{/*channel_id=*/77};
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};
  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  state->dispatcher = &dispatcher;
  state->connection = &connection_;

  auto client_transport =
      std::make_unique<ClientTransport>(ClientTransportContext{
          .transport =
              transport::any_transport{LoopbackTransport{any_executor_, state}},
          .endpoint_url = "opc.tcp://localhost:4840",
          .limits = {},
      });
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{*client_transport};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client.Open()).good());

  // CreateSession.
  const auto created = Call<CreateSessionResponse>(client, opcua::NodeId{},
                                                   CreateSessionRequest{});
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);
  const auto token = created.authentication_token;
  ASSERT_FALSE(token.is_null());

  // ActivateSession (anonymous).
  const auto activated = Call<ActivateSessionResponse>(
      client, token, ActivateSessionRequest{.allow_anonymous = true});
  ASSERT_EQ(activated.status.code(), opcua::StatusCode::Good);

  // Read.
  const auto read = Call<ua::ReadResponse>(
      client, token,
      ua::ReadRequest{
          .nodes_to_read = {{.node_id = opcua::NodeId{1, 2},
                             .attribute_id = static_cast<opcua::UInt32>(
                                 opcua::AttributeId::Value)}}});
  ASSERT_EQ(read.response_header.service_result.code(),
            opcua::StatusCode::Good);
  ASSERT_EQ(read.results.size(), 1u);
  EXPECT_EQ(read.results[0].value, opcua::Variant{opcua::Int32{42}});

  // Browse.
  const auto browse = Call<ua::BrowseResponse>(
      client, token,
      ua::BrowseRequest{.nodes_to_browse = {{.node_id = opcua::NodeId{1, 2}}}});
  EXPECT_EQ(browse.response_header.service_result.code(),
            opcua::StatusCode::Good);

  // CloseSession.
  const auto closed =
      Call<CloseSessionResponse>(client, token, CloseSessionRequest{});
  EXPECT_EQ(closed.status.code(), opcua::StatusCode::Good);
}

}  // namespace
}  // namespace opcua::binary
