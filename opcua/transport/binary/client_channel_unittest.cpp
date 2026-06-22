#include "opcua/client/client_channel.h"
#include "opcua/transport/binary/client_connection.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/async_completion.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/service_codec.h"
#include "transport/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace opcua::binary {
namespace {

struct ScriptedState {
  std::deque<std::string> incoming;
  std::vector<std::string> writes;
  bool opened = false;
  bool closed = false;
};

class ScriptedTransport {
 public:
  ScriptedTransport(transport::executor executor,
                    std::shared_ptr<ScriptedState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedTransport(ScriptedTransport&&) = default;
  ScriptedTransport& operator=(ScriptedTransport&&) = default;
  ScriptedTransport(const ScriptedTransport&) = delete;
  ScriptedTransport& operator=(const ScriptedTransport&) = delete;

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
    state_->writes.emplace_back(data.begin(), data.end());
    co_return data.size();
  }
  std::string name() const { return "ScriptedTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<ScriptedState> state_;
};

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

std::vector<char> BuildOpenResponseFrame(std::uint32_t channel_id,
                                         std::uint32_t token_id) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 1,
                          .service_result = opcua::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = channel_id,
                         .token_id = token_id,
                         .created_at = 0,
                         .revised_lifetime = 60000},
      .server_nonce = {},
  };
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id,
      .asymmetric_security_header =
          AsymmetricSecurityHeader{
              .security_policy_uri = std::string{kSecurityPolicyNone},
              .sender_certificate = {},
              .receiver_certificate_thumbprint = {},
          },
      .sequence_header = {.sequence_number = 1, .request_id = 1},
      .body = EncodeOpenSecureChannelResponseBody(response),
  };
  return EncodeSecureConversationMessage(message);
}

// Wraps a service-response body (produced by EncodeServiceResponse)
// into a SecureMessage frame at the given channel/token/request_id.
std::vector<char> BuildServiceResponseFrame(std::uint32_t channel_id,
                                            std::uint32_t token_id,
                                            std::uint32_t request_id,
                                            std::vector<char> body) {
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          SymmetricSecurityHeader{.token_id = token_id},
      .sequence_header = {.sequence_number = 2, .request_id = request_id},
      .body = std::move(body),
  };
  return EncodeSecureConversationMessage(message);
}

class BlockingConnection final : public opcua::ClientConnection {
 public:
  explicit BlockingConnection(opcua::AnyExecutor executor)
      : executor_{std::move(executor)}, first_send_released_{executor_} {}

  opcua::Awaitable<opcua::Status> Open() override {
    co_return opcua::Status{opcua::StatusCode::Good};
  }

  opcua::Awaitable<opcua::Status> Close() override {
    co_return opcua::Status{opcua::StatusCode::Good};
  }

  std::uint32_t NextRequestId() override { return next_request_id_++; }

  opcua::Awaitable<opcua::Status> SendRequest(
      std::uint32_t request_id,
      const RequestMessage& message,
      const opcua::NodeId& authentication_token) override {
    ++active_sends_;
    max_active_sends_ = std::max(max_active_sends_, active_sends_);
    request_ids.push_back(request_id);
    request_handles.push_back(message.request_handle);

    const auto send_index = send_count_++;
    if (block_first_send_ && send_index == 0) {
      co_await first_send_released_.Wait();
    }

    --active_sends_;
    co_return opcua::Status{opcua::StatusCode::Good};
  }

  opcua::Awaitable<opcua::StatusOr<ClientResponseFrame>> ReadResponse()
      override {
    co_return opcua::StatusOr<ClientResponseFrame>{
        opcua::Status{opcua::StatusCode::Bad_Disconnected}};
  }

  void ReleaseFirstSend() { first_send_released_.Complete(); }

  bool block_first_send_ = false;
  int active_sends_ = 0;
  int max_active_sends_ = 0;
  int send_count_ = 0;
  std::vector<std::uint32_t> request_ids;
  std::vector<std::uint32_t> request_handles;

 private:
  opcua::AnyExecutor executor_;
  opcua::base::AsyncCompletion first_send_released_;
  std::uint32_t next_request_id_ = 1;
};

class ClientChannelTest : public ::testing::Test {
 protected:
  static constexpr std::uint32_t kChannelId = 42;
  static constexpr std::uint32_t kTokenId = 1;

  std::unique_ptr<ClientTransport> MakeClientTransport(
      const std::shared_ptr<ScriptedState>& state) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport =
            transport::any_transport{ScriptedTransport{any_executor_, state}},
        .endpoint_url = "opc.tcp://localhost:4840",
        .limits = {},
    });
  }

  void PrimeAcknowledge(const std::shared_ptr<ScriptedState>& state) {
    state->incoming.push_back(AsString(EncodeAcknowledgeMessage(
        {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
  }

  // Drives transport.Connect + secure_channel.Open, leaving the channel in
  // the opened state with the channel_id/token_id above. The test should
  // push service response frames into state->incoming AFTER calling this.
  void OpenChannel(const std::shared_ptr<ScriptedState>& state,
                   ClientTransport& transport,
                   ClientSecureChannel& secure_channel) {
    PrimeAcknowledge(state);
    state->incoming.push_back(
        AsString(BuildOpenResponseFrame(kChannelId, kTokenId)));
    ASSERT_TRUE(opcua::WaitAwaitable(executor_, transport.Connect()).good());
    ASSERT_TRUE(opcua::WaitAwaitable(executor_, secure_channel.Open()).good());
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(ClientChannelTest, RequestHandlesAreMonotonic) {
  auto state = std::make_shared<ScriptedState>();
  PrimeAcknowledge(state);
  auto transport = MakeClientTransport(state);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, transport->Connect()).good());
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  EXPECT_EQ(channel.NextRequestHandle(), 1u);
  EXPECT_EQ(channel.NextRequestHandle(), 2u);
}

TEST_F(ClientChannelTest, CallReadReturnsTypedResponse) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);

  const std::uint32_t request_handle = 7;
  // The server will see the client's secure-channel request_id == 2 (1 was
  // consumed by OPN). Queue a Read response against request_id 2.
  ReadResponse server_reply{
      .status = opcua::StatusCode::Good,
      .results = {opcua::DataValue{
          opcua::Variant{std::int32_t{42}}, {}, {}, {}}},
  };
  const auto encoded_body =
      EncodeServiceResponse(request_handle, ResponseBody{server_reply});
  ASSERT_TRUE(encoded_body.has_value());
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      kChannelId, kTokenId, /*request_id=*/2, *encoded_body)));

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  const auto result = opcua::WaitAwaitable(
      executor_,
      channel.Call(request_handle, RequestBody{ReadRequest{.inputs = {}}}));
  ASSERT_TRUE(result.ok());
  const auto* typed = std::get_if<ReadResponse>(&result.value());
  ASSERT_NE(typed, nullptr);
  EXPECT_TRUE(typed->status.good());
  ASSERT_EQ(typed->results.size(), 1u);
  EXPECT_EQ(typed->results[0].value, (opcua::Variant{std::int32_t{42}}));
}

TEST_F(ClientChannelTest, CallWriteReturnsStatusCodes) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);

  const std::uint32_t request_handle = 8;
  WriteResponse server_reply{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good,
                  opcua::StatusCode::Bad_WrongAttributeId},
  };
  const auto encoded_body =
      EncodeServiceResponse(request_handle, ResponseBody{server_reply});
  ASSERT_TRUE(encoded_body.has_value());
  state->incoming.push_back(AsString(
      BuildServiceResponseFrame(kChannelId, kTokenId, 2, *encoded_body)));

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  const auto result = opcua::WaitAwaitable(
      executor_,
      channel.Call(request_handle, RequestBody{WriteRequest{.inputs = {}}}));
  ASSERT_TRUE(result.ok());
  const auto* typed = std::get_if<WriteResponse>(&result.value());
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->results.size(), 2u);
  EXPECT_EQ(typed->results[0], opcua::StatusCode::Good);
  EXPECT_EQ(typed->results[1], opcua::StatusCode::Bad_WrongAttributeId);
}

TEST_F(ClientChannelTest, CallRejectsMismatchedRequestHandle) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);

  // Server response encoded with request_handle=999 while the client sends
  // request_handle=7.
  WriteResponse stray{.status = opcua::StatusCode::Good};
  const auto encoded_body = EncodeServiceResponse(999, ResponseBody{stray});
  ASSERT_TRUE(encoded_body.has_value());
  state->incoming.push_back(AsString(
      BuildServiceResponseFrame(kChannelId, kTokenId, 2, *encoded_body)));

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  const auto result = opcua::WaitAwaitable(
      executor_, channel.Call(/*request_handle=*/7,
                              RequestBody{WriteRequest{.inputs = {}}}));
  EXPECT_FALSE(result.ok());
}

TEST_F(ClientChannelTest, CallReturnsBadOnConnectionClosed) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);
  // No service response queued — the ReadFrame will see EOF and report bad.

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  const auto result = opcua::WaitAwaitable(
      executor_, channel.Call(1, RequestBody{ReadRequest{.inputs = {}}}));
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.status().bad());
}

TEST_F(ClientChannelTest, CallIncludesAuthenticationTokenInRequestHeader) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);

  // Queue any response so the Call completes.
  const std::uint32_t request_handle = 11;
  const auto encoded_body = EncodeServiceResponse(
      request_handle,
      ResponseBody{CloseSessionResponse{.status = opcua::StatusCode::Good}});
  ASSERT_TRUE(encoded_body.has_value());
  state->incoming.push_back(AsString(
      BuildServiceResponseFrame(kChannelId, kTokenId, 2, *encoded_body)));

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  channel.set_authentication_token(opcua::NodeId{0xABCDEF});
  EXPECT_EQ(channel.authentication_token(), opcua::NodeId{0xABCDEF});

  [[maybe_unused]] auto unused = opcua::WaitAwaitable(
      executor_,
      channel.Call(request_handle, RequestBody{CloseSessionRequest{}}));

  // The outgoing SecureMessage is writes[2] (after Hello + OPN). Its body
  // starts with the OPC UA request header, whose first field is the
  // authentication_token NodeId. Verify by decoding and reading the
  // request header directly.
  ASSERT_GE(state->writes.size(), 3u);
  const auto bytes =
      std::vector<char>{state->writes[2].begin(), state->writes[2].end()};
  const auto decoded = DecodeSecureConversationMessage(bytes);
  ASSERT_TRUE(decoded.has_value());
  const auto request = DecodeServiceRequest(decoded->body);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->header.authentication_token, opcua::NodeId{0xABCDEF});
  EXPECT_EQ(request->header.request_handle, request_handle);
}

TEST_F(ClientChannelTest, SplitSendReceiveBuffersOutOfOrderResponses) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};

  const std::uint32_t first_handle = 21;
  const std::uint32_t second_handle = 22;
  auto first_request_id = opcua::WaitAwaitable(
      executor_,
      channel.Send(first_handle, RequestBody{ReadRequest{.inputs = {}}}));
  auto second_request_id = opcua::WaitAwaitable(
      executor_,
      channel.Send(second_handle, RequestBody{WriteRequest{.inputs = {}}}));
  ASSERT_TRUE(first_request_id.ok());
  ASSERT_TRUE(second_request_id.ok());
  EXPECT_EQ(*first_request_id, 2u);
  EXPECT_EQ(*second_request_id, 3u);

  const auto second_body = EncodeServiceResponse(
      second_handle,
      ResponseBody{WriteResponse{.status = opcua::StatusCode::Good,
                                 .results = {opcua::StatusCode::Good}}});
  const auto first_body = EncodeServiceResponse(
      first_handle, ResponseBody{ReadResponse{
                        .status = opcua::StatusCode::Good,
                        .results = {opcua::DataValue{
                            opcua::Variant{std::int32_t{7}}, {}, {}, {}}}}});
  ASSERT_TRUE(second_body.has_value());
  ASSERT_TRUE(first_body.has_value());
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      kChannelId, kTokenId, *second_request_id, *second_body)));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      kChannelId, kTokenId, *first_request_id, *first_body)));

  const auto first = opcua::WaitAwaitable(
      executor_, channel.Receive(*first_request_id, first_handle));
  ASSERT_TRUE(first.ok());
  const auto* read = std::get_if<ReadResponse>(&first.value());
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->results.size(), 1u);
  EXPECT_EQ(read->results[0].value, (opcua::Variant{std::int32_t{7}}));

  const auto second = opcua::WaitAwaitable(
      executor_, channel.Receive(*second_request_id, second_handle));
  ASSERT_TRUE(second.ok());
  const auto* write = std::get_if<WriteResponse>(&second.value());
  ASSERT_NE(write, nullptr);
  ASSERT_EQ(write->results.size(), 1u);
  EXPECT_EQ(write->results[0], opcua::StatusCode::Good);
}

TEST_F(ClientChannelTest, ConcurrentReceivesCompleteOutOfOrderResponses) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  OpenChannel(state, *transport, secure_channel);

  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};

  const std::uint32_t first_handle = 41;
  const std::uint32_t second_handle = 42;
  auto first_request_id = opcua::WaitAwaitable(
      executor_,
      channel.Send(first_handle, RequestBody{ReadRequest{.inputs = {}}}));
  auto second_request_id = opcua::WaitAwaitable(
      executor_,
      channel.Send(second_handle, RequestBody{WriteRequest{.inputs = {}}}));
  ASSERT_TRUE(first_request_id.ok());
  ASSERT_TRUE(second_request_id.ok());

  const auto second_body = EncodeServiceResponse(
      second_handle,
      ResponseBody{WriteResponse{.status = opcua::StatusCode::Good,
                                 .results = {opcua::StatusCode::Good}}});
  const auto first_body = EncodeServiceResponse(
      first_handle, ResponseBody{ReadResponse{
                        .status = opcua::StatusCode::Good,
                        .results = {opcua::DataValue{
                            opcua::Variant{std::int32_t{8}}, {}, {}, {}}}}});
  ASSERT_TRUE(second_body.has_value());
  ASSERT_TRUE(first_body.has_value());
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      kChannelId, kTokenId, *second_request_id, *second_body)));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      kChannelId, kTokenId, *first_request_id, *first_body)));

  auto first = opcua::StartAwaitable(
      executor_, channel.Receive(*first_request_id, first_handle));
  auto second = opcua::StartAwaitable(
      executor_, channel.Receive(*second_request_id, second_handle));
  Drain(executor_);
  ASSERT_TRUE(first->done);
  ASSERT_TRUE(second->done);

  const auto first_response = opcua::WaitResult(executor_, first);
  const auto second_response = opcua::WaitResult(executor_, second);
  ASSERT_TRUE(first_response.ok());
  ASSERT_TRUE(second_response.ok());

  const auto* read = std::get_if<ReadResponse>(&first_response.value());
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->results.size(), 1u);
  EXPECT_EQ(read->results[0].value, (opcua::Variant{std::int32_t{8}}));

  const auto* write = std::get_if<WriteResponse>(&second_response.value());
  ASSERT_NE(write, nullptr);
  ASSERT_EQ(write->results.size(), 1u);
  EXPECT_EQ(write->results[0], opcua::StatusCode::Good);
}

TEST_F(ClientChannelTest, ConcurrentSendsAreSerializedOnConnectionSend) {
  BlockingConnection connection{any_executor_};
  connection.block_first_send_ = true;
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};

  auto first = opcua::StartAwaitable(
      executor_, channel.Send(31, RequestBody{ReadRequest{.inputs = {}}}));
  Drain(executor_);
  ASSERT_FALSE(first->done);
  ASSERT_EQ(connection.request_ids, (std::vector<std::uint32_t>{1}));

  auto second = opcua::StartAwaitable(
      executor_, channel.Send(32, RequestBody{WriteRequest{.inputs = {}}}));
  Drain(executor_);
  ASSERT_FALSE(second->done);
  EXPECT_EQ(connection.request_ids, (std::vector<std::uint32_t>{1}));
  EXPECT_EQ(connection.max_active_sends_, 1);

  connection.ReleaseFirstSend();
  Drain(executor_);

  const auto first_id = opcua::WaitResult(executor_, first);
  const auto second_id = opcua::WaitResult(executor_, second);
  ASSERT_TRUE(first_id.ok());
  ASSERT_TRUE(second_id.ok());
  EXPECT_EQ(*first_id, 1u);
  EXPECT_EQ(*second_id, 2u);
  EXPECT_EQ(connection.request_ids, (std::vector<std::uint32_t>{1, 2}));
  EXPECT_EQ(connection.request_handles, (std::vector<std::uint32_t>{31, 32}));
  EXPECT_EQ(connection.max_active_sends_, 1);
}

}  // namespace
}  // namespace opcua::binary
