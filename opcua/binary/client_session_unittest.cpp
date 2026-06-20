#include "opcua/client_protocol_session.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/binary/client_connection.h"
#include "opcua/binary/client_secure_channel.h"
#include "opcua/binary/client_transport.h"
#include "opcua/binary/secure_channel.h"
#include "opcua/binary/service_codec.h"
#include "opcua/client_channel.h"
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

// --- Response frame builders (each mirrors what a live server would send
// for the given client-visible request_id) -----------------------------

constexpr std::uint32_t kChannelId = 42;
constexpr std::uint32_t kTokenId = 1;

std::vector<char> BuildOpenResponseFrame() {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 1,
                          .service_result = scada::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = kChannelId,
                         .token_id = kTokenId,
                         .created_at = 0,
                         .revised_lifetime = 60000},
      .server_nonce = {},
  };
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kChannelId,
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

std::vector<char> BuildServiceResponseFrame(std::uint32_t request_id,
                                            std::uint32_t request_handle,
                                            ResponseBody body) {
  const auto encoded = EncodeServiceResponse(request_handle, std::move(body));
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kChannelId,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          SymmetricSecurityHeader{.token_id = kTokenId},
      .sequence_header = {.sequence_number = request_id + 1,
                          .request_id = request_id},
      .body = encoded.value(),
  };
  return EncodeSecureConversationMessage(message);
}

// Encodes one service response and splits it across two MessageChunks: an
// intermediate 'C' chunk and a final 'F' chunk (same request_id), mirroring how
// a server fragments a response larger than the negotiated chunk size.
std::pair<std::vector<char>, std::vector<char>>
BuildChunkedServiceResponseFrames(std::uint32_t request_id,
                                  std::uint32_t request_handle,
                                  ResponseBody body) {
  const auto encoded =
      EncodeServiceResponse(request_handle, std::move(body)).value();
  const std::size_t split = encoded.size() / 2;
  const auto make_frame = [&](char chunk_type, std::uint32_t sequence_number,
                              std::size_t from, std::size_t to) {
    const SecureConversationMessage message{
        .frame_header = {.message_type = MessageType::SecureMessage,
                         .chunk_type = chunk_type,
                         .message_size = 0},
        .secure_channel_id = kChannelId,
        .asymmetric_security_header = std::nullopt,
        .symmetric_security_header =
            SymmetricSecurityHeader{.token_id = kTokenId},
        .sequence_header = {.sequence_number = sequence_number,
                            .request_id = request_id},
        .body = std::vector<char>(
            encoded.begin() + static_cast<std::ptrdiff_t>(from),
            encoded.begin() + static_cast<std::ptrdiff_t>(to)),
    };
    return EncodeSecureConversationMessage(message);
  };
  return {make_frame('C', request_id + 1, 0, split),
          make_frame('F', request_id + 2, split, encoded.size())};
}

class ClientProtocolSessionTest : public ::testing::Test {
 protected:
  // Every client call advances the secure channel's request_id by 1. The
  // helpers below queue responses for specific request_ids so the reader
  // sees them in the expected order:
  //   request_id 1 -> OPN response
  //   request_id 2 -> first service call (CreateSession during Create())
  //   request_id 3 -> ActivateSession
  //   request_id 4+ -> post-Create service calls
  void PrimeConnectAndOpen(const std::shared_ptr<ScriptedState>& state) {
    state->incoming.push_back(AsString(EncodeAcknowledgeMessage(
        {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
    state->incoming.push_back(AsString(BuildOpenResponseFrame()));
  }

  // Queues the Create + Activate responses the session needs to finish
  // Create(). Returns the authentication_token the client will see.
  scada::NodeId PrimeSessionEstablishment(
      const std::shared_ptr<ScriptedState>& state) {
    const scada::NodeId session_id{111};
    const scada::NodeId auth_token{0xABCDEF};
    state->incoming.push_back(AsString(BuildServiceResponseFrame(
        /*request_id=*/2, /*request_handle=*/1,
        ResponseBody{CreateSessionResponse{
            .status = scada::StatusCode::Good,
            .session_id = session_id,
            .authentication_token = auth_token,
            .server_nonce = scada::ByteString{},
            .revised_timeout = base::TimeDelta::FromSeconds(60),
        }})));
    state->incoming.push_back(AsString(BuildServiceResponseFrame(
        /*request_id=*/3, /*request_handle=*/2,
        ResponseBody{
            ActivateSessionResponse{.status = scada::StatusCode::Good}})));
    return auth_token;
  }

  TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(ClientProtocolSessionTest, CreateRunsCreateAndActivate) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  const auto auth_token = PrimeSessionEstablishment(state);

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};

  const auto status = WaitAwaitable(executor_, session.Create());
  ASSERT_TRUE(status.good());
  EXPECT_TRUE(session.is_active());
  EXPECT_EQ(session.session_id(), scada::NodeId{111});
  EXPECT_EQ(session.authentication_token(), auth_token);
}

TEST_F(ClientProtocolSessionTest, CreateRejectsServerCertificateMismatch) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  // CreateSession returns a certificate that differs from what the client
  // selected during discovery. No ActivateSession is primed: the session must
  // fail before reaching it.
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSessionResponse{
          .status = scada::StatusCode::Good,
          .session_id = scada::NodeId{111},
          .authentication_token = scada::NodeId{0xABCDEF},
          .server_nonce = scada::ByteString{},
          .server_certificate = scada::ByteString{'a', 'c', 't', 'u', 'a', 'l'},
          .revised_timeout = base::TimeDelta::FromSeconds(60),
      }})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};

  ClientProtocolSession::ClientCredentials credentials;
  credentials.expected_server_certificate =
      scada::ByteString{'e', 'x', 'p', 'e', 'c', 't', 'e', 'd'};
  const auto status =
      WaitAwaitable(executor_, session.Create({}, {}, std::move(credentials)));
  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(session.is_active());
}

TEST_F(ClientProtocolSessionTest, CreateAcceptsMatchingServerCertificate) {
  const scada::ByteString server_certificate{'s', 'e', 'r', 'v', 'e', 'r'};
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSessionResponse{
          .status = scada::StatusCode::Good,
          .session_id = scada::NodeId{111},
          .authentication_token = scada::NodeId{0xABCDEF},
          .server_nonce = scada::ByteString{},
          .server_certificate = server_certificate,
          .revised_timeout = base::TimeDelta::FromSeconds(60),
      }})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/3, /*request_handle=*/2,
      ResponseBody{
          ActivateSessionResponse{.status = scada::StatusCode::Good}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};

  ClientProtocolSession::ClientCredentials credentials;
  credentials.expected_server_certificate = server_certificate;
  const auto status =
      WaitAwaitable(executor_, session.Create({}, {}, std::move(credentials)));
  EXPECT_TRUE(status.good());
  EXPECT_TRUE(session.is_active());
}

TEST_F(ClientProtocolSessionTest, CreatePropagatesCreateSessionBadStatus) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSessionResponse{
          .status = scada::StatusCode::Bad_WrongLoginCredentials}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};

  const auto status = WaitAwaitable(executor_, session.Create());
  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(session.is_active());
}

TEST_F(ClientProtocolSessionTest, ReadReturnsDataValuesOnSuccess) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  // Read is the next call after Activate, so request_id=4, request_handle=3.
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{ReadResponse{
          .status = scada::StatusCode::Good,
          .results = {scada::DataValue{
              scada::Variant{std::int32_t{7}}, {}, {}, {}}},
      }})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto read = WaitAwaitable(
      executor_, session.Read(std::vector<scada::ReadValueId>{
                     {.node_id = scada::NodeId{1},
                      .attribute_id = scada::AttributeId::Value}}));
  ASSERT_TRUE(read.ok());
  ASSERT_EQ(read->size(), 1u);
  EXPECT_EQ((*read)[0].value, (scada::Variant{std::int32_t{7}}));
}

TEST_F(ClientProtocolSessionTest, ReadReassemblesMultiChunkResponse) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  // The Read response (two values, so it is large enough to split) arrives as a
  // 'C' chunk followed by an 'F' chunk; the channel must reassemble them.
  auto chunks = BuildChunkedServiceResponseFrames(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{ReadResponse{
          .status = scada::StatusCode::Good,
          .results =
              {scada::DataValue{scada::Variant{std::int32_t{7}}, {}, {}, {}},
               scada::DataValue{scada::Variant{std::int32_t{42}}, {}, {}, {}}},
      }});
  state->incoming.push_back(AsString(chunks.first));
  state->incoming.push_back(AsString(chunks.second));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto read = WaitAwaitable(
      executor_, session.Read(std::vector<scada::ReadValueId>{
                     {.node_id = scada::NodeId{1},
                      .attribute_id = scada::AttributeId::Value}}));
  ASSERT_TRUE(read.ok());
  ASSERT_EQ(read->size(), 2u);
  EXPECT_EQ((*read)[0].value, (scada::Variant{std::int32_t{7}}));
  EXPECT_EQ((*read)[1].value, (scada::Variant{std::int32_t{42}}));
}

TEST_F(ClientProtocolSessionTest, WriteReturnsStatusCodes) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{WriteResponse{.status = scada::StatusCode::Good,
                                 .results = {scada::StatusCode::Good}}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto write = WaitAwaitable(
      executor_, session.Write(std::vector<scada::WriteValue>{
                     {.node_id = scada::NodeId{1},
                      .attribute_id = scada::AttributeId::Value,
                      .value = scada::Variant{std::int32_t{99}}}}));
  ASSERT_TRUE(write.ok());
  ASSERT_EQ(write->size(), 1u);
  EXPECT_EQ((*write)[0], scada::StatusCode::Good);
}

TEST_F(ClientProtocolSessionTest, AddNodesReturnsAddedNodeIds) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{
          AddNodesResponse{.status = scada::StatusCode::Good,
                           .results = {scada::AddNodesResult{
                               .status_code = scada::StatusCode::Good,
                               .added_node_id = scada::NodeId{101, 6}}}}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto add = WaitAwaitable(
      executor_, session.AddNodes(std::vector<scada::AddNodesItem>{
                     {.requested_id = scada::NodeId{101, 6},
                      .parent_id = scada::NodeId{12, 7},
                      .node_class = scada::NodeClass::Object,
                      .type_definition_id = scada::NodeId{170, 7}}}));
  ASSERT_TRUE(add.ok());
  ASSERT_EQ(add->size(), 1u);
  EXPECT_EQ((*add)[0].status_code, scada::StatusCode::Good);
  EXPECT_EQ((*add)[0].added_node_id, (scada::NodeId{101, 6}));
}

TEST_F(ClientProtocolSessionTest, DeleteNodesReturnsStatusCodes) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{
          DeleteNodesResponse{.status = scada::StatusCode::Good,
                              .results = {scada::StatusCode::Good}}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto deleted = WaitAwaitable(
      executor_, session.DeleteNodes(std::vector<scada::DeleteNodesItem>{
                     {.node_id = scada::NodeId{101, 6}}}));
  ASSERT_TRUE(deleted.ok());
  ASSERT_EQ(deleted->size(), 1u);
  EXPECT_EQ((*deleted)[0], scada::StatusCode::Good);
}

TEST_F(ClientProtocolSessionTest, AddReferencesReturnsStatusCodes) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{
          AddReferencesResponse{.status = scada::StatusCode::Good,
                                .results = {scada::StatusCode::Good}}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto added = WaitAwaitable(
      executor_,
      session.AddReferences(std::vector<scada::AddReferencesItem>{
          {.source_node_id = scada::NodeId{1},
           .reference_type_id = scada::NodeId{2},
           .target_node_id = scada::ExpandedNodeId{scada::NodeId{3}}}}));
  ASSERT_TRUE(added.ok());
  ASSERT_EQ(added->size(), 1u);
  EXPECT_EQ((*added)[0], scada::StatusCode::Good);
}

TEST_F(ClientProtocolSessionTest, DeleteReferencesReturnsStatusCodes) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{
          DeleteReferencesResponse{.status = scada::StatusCode::Good,
                                   .results = {scada::StatusCode::Good}}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto deleted = WaitAwaitable(
      executor_,
      session.DeleteReferences(std::vector<scada::DeleteReferencesItem>{
          {.source_node_id = scada::NodeId{1},
           .reference_type_id = scada::NodeId{2},
           .target_node_id = scada::ExpandedNodeId{scada::NodeId{3}}}}));
  ASSERT_TRUE(deleted.ok());
  ASSERT_EQ(deleted->size(), 1u);
  EXPECT_EQ((*deleted)[0], scada::StatusCode::Good);
}

TEST_F(ClientProtocolSessionTest, BrowseReturnsReferences) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{
          BrowseResponse{.status = scada::StatusCode::Good,
                         .results = {scada::BrowseResult{
                             .status_code = scada::StatusCode::Good,
                             .references = {scada::ReferenceDescription{
                                 .reference_type_id = scada::NodeId{35},
                                 .forward = true,
                                 .node_id = scada::NodeId{1000}}}}}}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto browse = WaitAwaitable(
      executor_, session.Browse(std::vector<scada::BrowseDescription>{
                     {.node_id = scada::NodeId{85}}}));
  ASSERT_TRUE(browse.ok());
  ASSERT_EQ(browse->size(), 1u);
  ASSERT_EQ((*browse)[0].references.size(), 1u);
  EXPECT_EQ((*browse)[0].references[0].node_id, scada::NodeId{1000});
}

TEST_F(ClientProtocolSessionTest, CallRoundTripsArguments) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  CallResponse server_reply;
  server_reply.results.push_back(
      {.status = scada::StatusCode::Good,
       .input_argument_results = {scada::StatusCode::Good},
       .output_arguments = {scada::Variant{std::int32_t{123}}}});
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3, ResponseBody{server_reply})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto call = WaitAwaitable(
      executor_, session.Call(scada::NodeId{2000}, scada::NodeId{3000},
                              {scada::Variant{std::int32_t{5}}}));
  ASSERT_TRUE(call.ok());
  EXPECT_TRUE(call->status.good());
  ASSERT_EQ(call->output_arguments.size(), 1u);
  EXPECT_EQ(call->output_arguments[0], (scada::Variant{std::int32_t{123}}));
}

TEST_F(ClientProtocolSessionTest, CloseRunsCloseSessionBestEffort) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{CloseSessionResponse{.status = scada::StatusCode::Good}})));

  ClientTransport transport{ClientTransportContext{
      .transport =
          transport::any_transport{ScriptedTransport{any_executor_, state}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  }};
  ClientSecureChannel secure_channel{transport};
  ClientConnection connection{
      {.transport = transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  ClientProtocolSession session{{.connection = connection, .channel = channel}};
  ASSERT_TRUE(WaitAwaitable(executor_, session.Create()).good());

  const auto status = WaitAwaitable(executor_, session.Close());
  EXPECT_TRUE(status.good());
  EXPECT_FALSE(session.is_active());
}

}  // namespace
}  // namespace opcua::binary
