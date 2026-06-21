#include "opcua/binary/tcp_connection.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/async_completion.h"
#include "transport/transport.h"

#include <gtest/gtest.h>

#include <deque>

namespace opcua::binary {
namespace {

struct StreamPeerState {
  std::deque<std::string> incoming;
  std::vector<std::string> writes;
  bool opened = false;
  bool closed = false;
};

class ScriptedStreamTransport {
 public:
  ScriptedStreamTransport(transport::executor executor,
                          std::shared_ptr<StreamPeerState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedStreamTransport(ScriptedStreamTransport&&) = default;
  ScriptedStreamTransport& operator=(ScriptedStreamTransport&&) = default;
  ScriptedStreamTransport(const ScriptedStreamTransport&) = delete;
  ScriptedStreamTransport& operator=(const ScriptedStreamTransport&) = delete;

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

  std::string name() const { return "ScriptedStreamTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<StreamPeerState> state_;
};

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

std::vector<char> Slice(const std::vector<char>& bytes,
                        std::size_t offset,
                        std::size_t count) {
  return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + count)};
}

std::vector<char> EncodeOpenRequestBody(
    std::uint32_t request_handle,
    SecurityTokenRequestType request_type =
        SecurityTokenRequestType::Issue,
    MessageSecurityMode security_mode =
        MessageSecurityMode::None,
    std::uint32_t requested_lifetime = 60000) {
  auto append_u8 = [](std::vector<char>& bytes, std::uint8_t value) {
    bytes.push_back(static_cast<char>(value));
  };
  auto append_u16 = [](std::vector<char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xff));
    bytes.push_back(static_cast<char>((value >> 8) & 0xff));
  };
  auto append_u32 = [](std::vector<char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<char>(value & 0xff));
    bytes.push_back(static_cast<char>((value >> 8) & 0xff));
    bytes.push_back(static_cast<char>((value >> 16) & 0xff));
    bytes.push_back(static_cast<char>((value >> 24) & 0xff));
  };
  auto append_i32 = [&](std::vector<char>& bytes, std::int32_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(value));
  };
  auto append_i64 = [](std::vector<char>& bytes, std::int64_t value) {
    const auto raw = static_cast<std::uint64_t>(value);
    for (int i = 0; i < 8; ++i) {
      bytes.push_back(static_cast<char>((raw >> (8 * i)) & 0xff));
    }
  };
  auto append_string = [&](std::vector<char>& bytes, std::string_view value) {
    append_i32(bytes, static_cast<std::int32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  };
  auto append_bytes = [&](std::vector<char>& bytes,
                          const opcua::ByteString& value) {
    append_i32(bytes, static_cast<std::int32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  };
  auto append_nodeid = [&](std::vector<char>& bytes, std::uint32_t id) {
    if (id == 0) {
      append_u8(bytes, 0x00);
    } else {
      append_u8(bytes, 0x01);
      append_u8(bytes, 0);
      append_u16(bytes, static_cast<std::uint16_t>(id));
    }
  };
  auto append_extension = [&](std::vector<char>& bytes,
                              std::uint32_t type_id,
                              const std::vector<char>& payload) {
    append_nodeid(bytes, type_id);
    append_u8(bytes, 0x01);
    append_i32(bytes, static_cast<std::int32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  };

  std::vector<char> payload;
  append_nodeid(payload, 0);
  append_i64(payload, 0);
  append_u32(payload, request_handle);
  append_u32(payload, 0);
  append_string(payload, "");
  append_u32(payload, 0);
  append_nodeid(payload, 0);
  append_u8(payload, 0x00);
  append_u32(payload, 0);
  append_u32(payload, static_cast<std::uint32_t>(request_type));
  append_u32(payload, static_cast<std::uint32_t>(security_mode));
  append_bytes(payload, {});
  append_u32(payload, requested_lifetime);

  std::vector<char> body;
  append_extension(body, kOpenSecureChannelRequestEncodingId, payload);
  return body;
}

class TcpConnectionTest : public ::testing::Test {
 protected:
  void RunPeer(const std::shared_ptr<StreamPeerState>& peer,
               SecureFrameHandler handler =
                   [](std::vector<char>, SecureFrameContext)
                   -> opcua::Awaitable<std::optional<std::vector<char>>> {
                 co_return std::nullopt;
               }) {
    opcua::WaitAwaitable(executor_,
                  TcpConnection{
                      {.transport = transport::any_transport{
                           ScriptedStreamTransport{any_executor_, peer}},
                       .limits = server_limits_,
                       .on_secure_frame = std::move(handler)}}
                      .Run());
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  const TransportLimits server_limits_{
      .protocol_version = 0,
      .receive_buffer_size = 8192,
      .send_buffer_size = 4096,
      .max_message_size = 0,
      .max_chunk_count = 0,
  };
};

TEST_F(TcpConnectionTest,
       AcceptsHelloAndWritesAcknowledgeOverStreamTransport) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto hello = EncodeHelloMessage(
      {.protocol_version = 0,
       .receive_buffer_size = 16384,
       .send_buffer_size = 2048,
       .max_message_size = 0,
       .max_chunk_count = 0,
       .endpoint_url = "opc.tcp://localhost:4840"});
  peer->incoming.push_back(AsString(Slice(hello, 0, 10)));
  peer->incoming.push_back(AsString(Slice(hello, 10, hello.size() - 10)));

  RunPeer(peer);

  ASSERT_EQ(peer->writes.size(), 1u);
  const auto encoded = std::vector<char>{peer->writes[0].begin(),
                                         peer->writes[0].end()};
  const auto ack = DecodeAcknowledgeMessage(encoded);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->protocol_version, 0u);
  EXPECT_EQ(ack->receive_buffer_size, 2048u);
  EXPECT_EQ(ack->send_buffer_size, 4096u);
  EXPECT_TRUE(peer->closed);
}

TEST_F(TcpConnectionTest, RejectsSecondHelloMessage) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto hello = EncodeHelloMessage(
      {.protocol_version = 0,
       .receive_buffer_size = 16384,
       .send_buffer_size = 2048,
       .max_message_size = 0,
       .max_chunk_count = 0,
       .endpoint_url = "opc.tcp://localhost:4840"});
  peer->incoming.push_back(AsString(hello));
  peer->incoming.push_back(AsString(hello));

  RunPeer(peer);

  ASSERT_EQ(peer->writes.size(), 2u);
  const auto error = DecodeErrorMessage(
      std::vector<char>{peer->writes[1].begin(), peer->writes[1].end()});
  ASSERT_TRUE(error.has_value());
  EXPECT_TRUE(error->error.bad());
}

TEST_F(TcpConnectionTest,
       ForwardsSecureFramesAfterHelloToHandler) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto hello = EncodeHelloMessage(
      {.protocol_version = 0,
       .receive_buffer_size = 16384,
       .send_buffer_size = 2048,
       .max_message_size = 0,
       .max_chunk_count = 0,
       .endpoint_url = "opc.tcp://localhost:4840"});
  const auto open = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureOpen,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header = AsymmetricSecurityHeader{
           .security_policy_uri = std::string{kSecurityPolicyNone},
           .sender_certificate = {},
           .receiver_certificate_thumbprint = {},
       },
       .sequence_header = {.sequence_number = 1, .request_id = 1},
       .body = EncodeOpenRequestBody(1)});
  const std::vector<char> payload{'a', 'b', 'c', 'd'};
  const auto secure = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureMessage,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header =
           SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = 2, .request_id = 9},
       .body = payload});

  peer->incoming.push_back(AsString(hello));
  peer->incoming.push_back(AsString(open));
  peer->incoming.push_back(AsString(secure));

  std::vector<std::vector<char>> received_payloads;
  RunPeer(peer, [&](std::vector<char> frame, SecureFrameContext)
              -> opcua::Awaitable<std::optional<std::vector<char>>> {
    received_payloads.push_back(frame);
    co_return std::vector<char>{'o', 'k'};
  });

  ASSERT_EQ(received_payloads.size(), 1u);
  EXPECT_EQ(received_payloads[0], payload);
  ASSERT_EQ(peer->writes.size(), 3u);
  const auto response = DecodeSecureConversationMessage(
      std::vector<char>{peer->writes[2].begin(), peer->writes[2].end()});
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->frame_header.message_type,
            MessageType::SecureMessage);
  EXPECT_EQ(response->sequence_header.request_id, 9u);
  EXPECT_EQ(response->body, (std::vector<char>{'o', 'k'}));
}

namespace {

std::vector<char> NoneOpenFrame() {
  return EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {}},
       .sequence_header = {.sequence_number = 1, .request_id = 1},
       .body = EncodeOpenRequestBody(1)});
}

std::vector<char> SecureMessageChunk(char chunk_type,
                                     std::uint32_t sequence_number,
                                     std::uint32_t request_id,
                                     std::vector<char> body) {
  return EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureMessage,
                        .chunk_type = chunk_type,
                        .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header = SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = sequence_number,
                           .request_id = request_id},
       .body = std::move(body)});
}

}  // namespace

TEST_F(TcpConnectionTest, ReassemblesMultiChunkSecureMessage) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto hello = EncodeHelloMessage(
      {.protocol_version = 0,
       .receive_buffer_size = 16384,
       .send_buffer_size = 2048,
       .max_message_size = 0,
       .max_chunk_count = 0,
       .endpoint_url = "opc.tcp://localhost:4840"});
  peer->incoming.push_back(AsString(hello));
  peer->incoming.push_back(AsString(NoneOpenFrame()));
  peer->incoming.push_back(
      AsString(SecureMessageChunk('C', 2, 9, {'p', 'a', 'r', 't', '1'})));
  peer->incoming.push_back(
      AsString(SecureMessageChunk('C', 3, 9, {'p', 'a', 'r', 't', '2'})));
  peer->incoming.push_back(
      AsString(SecureMessageChunk('F', 4, 9, {'e', 'n', 'd'})));

  std::vector<std::vector<char>> received_payloads;
  RunPeer(peer, [&](std::vector<char> frame, SecureFrameContext)
              -> opcua::Awaitable<std::optional<std::vector<char>>> {
    received_payloads.push_back(frame);
    co_return std::nullopt;
  });

  // The handler sees exactly one reassembled payload, not three chunks.
  ASSERT_EQ(received_payloads.size(), 1u);
  EXPECT_EQ(received_payloads[0],
            (std::vector<char>{'p', 'a', 'r', 't', '1', 'p', 'a', 'r', 't', '2',
                               'e', 'n', 'd'}));
}

TEST_F(TcpConnectionTest, AbortChunkDiscardsPartialSecureMessage) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto hello = EncodeHelloMessage(
      {.protocol_version = 0,
       .receive_buffer_size = 16384,
       .send_buffer_size = 2048,
       .max_message_size = 0,
       .max_chunk_count = 0,
       .endpoint_url = "opc.tcp://localhost:4840"});
  peer->incoming.push_back(AsString(hello));
  peer->incoming.push_back(AsString(NoneOpenFrame()));
  peer->incoming.push_back(
      AsString(SecureMessageChunk('C', 2, 9, {'d', 'r', 'o', 'p'})));
  peer->incoming.push_back(AsString(SecureMessageChunk('A', 3, 9, {})));
  peer->incoming.push_back(
      AsString(SecureMessageChunk('F', 4, 10, {'k', 'e', 'p', 't'})));

  std::vector<std::vector<char>> received_payloads;
  RunPeer(peer, [&](std::vector<char> frame, SecureFrameContext)
              -> opcua::Awaitable<std::optional<std::vector<char>>> {
    received_payloads.push_back(frame);
    co_return std::nullopt;
  });

  // The aborted message is discarded; only the second message is delivered.
  ASSERT_EQ(received_payloads.size(), 1u);
  EXPECT_EQ(received_payloads[0], (std::vector<char>{'k', 'e', 'p', 't'}));
}

TEST_F(TcpConnectionTest,
       DispatchesLaterSecureFramesWhileFirstServiceFrameWaits) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto hello = EncodeHelloMessage(
      {.protocol_version = 0,
       .receive_buffer_size = 16384,
       .send_buffer_size = 2048,
       .max_message_size = 0,
       .max_chunk_count = 0,
       .endpoint_url = "opc.tcp://localhost:4840"});
  const auto open = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureOpen,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header = AsymmetricSecurityHeader{
           .security_policy_uri = std::string{kSecurityPolicyNone},
           .sender_certificate = {},
           .receiver_certificate_thumbprint = {},
       },
       .sequence_header = {.sequence_number = 1, .request_id = 1},
       .body = EncodeOpenRequestBody(1)});
  const std::vector<char> first_payload{'s', 'l', 'o', 'w'};
  const auto first_secure = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureMessage,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header =
           SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = 2, .request_id = 10},
       .body = first_payload});
  const std::vector<char> second_payload{'f', 'a', 's', 't'};
  const auto second_secure = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureMessage,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header =
           SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = 3, .request_id = 11},
       .body = second_payload});

  peer->incoming.push_back(AsString(hello));
  peer->incoming.push_back(AsString(open));
  peer->incoming.push_back(AsString(first_secure));
  peer->incoming.push_back(AsString(second_secure));

  opcua::base::AsyncCompletion release_first{any_executor_};
  std::vector<std::vector<char>> received_payloads;
  opcua::CoSpawn(any_executor_,
          [this, peer, release_first, &received_payloads]() mutable
              -> opcua::Awaitable<void> {
    co_await TcpConnection{
        {.transport = transport::any_transport{
             ScriptedStreamTransport{any_executor_, peer}},
         .limits = server_limits_,
         .on_secure_frame =
             [release_first, &received_payloads](
                 std::vector<char> frame, SecureFrameContext)
                 mutable -> opcua::Awaitable<std::optional<std::vector<char>>> {
           received_payloads.push_back(frame);
           if (frame == (std::vector<char>{'s', 'l', 'o', 'w'})) {
             co_await release_first.Wait();
             co_return std::vector<char>{'s', 'l', 'o', 'w', '-', 'o', 'k'};
           }
           co_return std::vector<char>{'f', 'a', 's', 't', '-', 'o', 'k'};
         }}}
        .Run();
  });

  for (int i = 0; i < 10; ++i) {
    executor_.Poll();
  }

  ASSERT_EQ(received_payloads.size(), 2u);
  EXPECT_EQ(received_payloads[0], first_payload);
  EXPECT_EQ(received_payloads[1], second_payload);
  ASSERT_GE(peer->writes.size(), 3u);
  const auto fast_response = DecodeSecureConversationMessage(
      std::vector<char>{peer->writes.back().begin(), peer->writes.back().end()});
  ASSERT_TRUE(fast_response.has_value());
  EXPECT_EQ(fast_response->sequence_header.request_id, 11u);
  EXPECT_EQ(fast_response->body,
            (std::vector<char>{'f', 'a', 's', 't', '-', 'o', 'k'}));

  release_first.Complete();
  for (int i = 0; i < 10; ++i) {
    executor_.Poll();
  }

  ASSERT_GE(peer->writes.size(), 4u);
  const auto slow_response = DecodeSecureConversationMessage(
      std::vector<char>{peer->writes.back().begin(), peer->writes.back().end()});
  ASSERT_TRUE(slow_response.has_value());
  EXPECT_EQ(slow_response->sequence_header.request_id, 10u);
  EXPECT_EQ(slow_response->body,
            (std::vector<char>{'s', 'l', 'o', 'w', '-', 'o', 'k'}));
}

TEST_F(TcpConnectionTest,
       RejectsSecureFrameBeforeHelloHandshake) {
  auto peer = std::make_shared<StreamPeerState>();
  auto secure = EncodeFrameHeader(
      {.message_type = MessageType::SecureOpen,
       .chunk_type = 'F',
       .message_size = 8});
  peer->incoming.push_back(AsString(secure));

  RunPeer(peer);

  ASSERT_EQ(peer->writes.size(), 1u);
  const auto error = DecodeErrorMessage(
      std::vector<char>{peer->writes[0].begin(), peer->writes[0].end()});
  ASSERT_TRUE(error.has_value());
  EXPECT_TRUE(error->error.bad());
}

}  // namespace
}  // namespace opcua::binary
