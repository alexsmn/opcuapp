#include "opcua/binary/secure_channel.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/binary/codec_utils.h"

#include <gtest/gtest.h>

namespace opcua::binary {
namespace {

TestExecutor executor_;

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
                          const scada::ByteString& value) {
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

std::vector<char> EncodeCloseRequestBody(std::uint32_t request_handle) {
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

  std::vector<char> body;
  append_extension(body, kCloseSecureChannelRequestEncodingId, payload);
  return body;
}

TEST(SecureChannelTest, DecodesAndEncodesOpenRequestResponse) {
  const auto body = EncodeOpenRequestBody(77);
  const auto request = DecodeOpenSecureChannelRequestBody(body);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->request_header.request_handle, 77u);
  EXPECT_EQ(request->security_mode, MessageSecurityMode::None);

  const auto response_body = EncodeOpenSecureChannelResponseBody(
      {.response_header = {.request_handle = 77,
                           .service_result = scada::StatusCode::Good},
       .server_protocol_version = 0,
       .security_token =
           {.channel_id = 91, .token_id = 4, .created_at = 0, .revised_lifetime = 60000},
       .server_nonce = {}});
  EXPECT_FALSE(response_body.empty());
}

TEST(SecureChannelTest, OpenRequestProducesOpenResponseFrame) {
  SecureChannel channel{91};
  const auto frame = EncodeSecureConversationMessage(
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
       .sequence_header = {.sequence_number = 1, .request_id = 8},
       .body = EncodeOpenRequestBody(55)});

  const auto result = WaitAwaitable(executor_, channel.HandleFrame(frame));
  ASSERT_TRUE(result.outbound_frame.has_value());
  EXPECT_FALSE(result.close_transport);
  EXPECT_TRUE(channel.opened());

  const auto response = DecodeSecureConversationMessage(*result.outbound_frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->frame_header.message_type,
            MessageType::SecureOpen);
  EXPECT_EQ(response->secure_channel_id, 91u);
  EXPECT_EQ(response->sequence_header.request_id, 8u);
}

TEST(SecureChannelTest, RejectsUnsupportedSecurityModeInOpen) {
  SecureChannel channel{19};
  const auto frame = EncodeSecureConversationMessage(
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
       .sequence_header = {.sequence_number = 1, .request_id = 2},
       .body = EncodeOpenRequestBody(
           12, SecurityTokenRequestType::Issue,
           MessageSecurityMode::Sign)});

  const auto result = WaitAwaitable(executor_, channel.HandleFrame(frame));
  ASSERT_TRUE(result.outbound_frame.has_value());
  EXPECT_FALSE(channel.opened());
}

TEST(SecureChannelTest, RoutesMessageBodyAfterOpen) {
  SecureChannel channel{7};
  const auto open_frame = EncodeSecureConversationMessage(
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
       .sequence_header = {.sequence_number = 1, .request_id = 4},
       .body = EncodeOpenRequestBody(44)});
  ASSERT_TRUE(WaitAwaitable(executor_, channel.HandleFrame(open_frame))
                  .outbound_frame.has_value());

  const std::vector<char> payload{'x', 'y', 'z'};
  const auto msg_frame = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureMessage,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 7,
       .symmetric_security_header = SymmetricSecurityHeader{
           .token_id = channel.token_id()},
       .sequence_header = {.sequence_number = 2, .request_id = 5},
       .body = payload});

  const auto result = WaitAwaitable(executor_, channel.HandleFrame(msg_frame));
  ASSERT_TRUE(result.service_payload.has_value());
  EXPECT_EQ(*result.service_payload, payload);
  ASSERT_TRUE(result.request_id.has_value());
  EXPECT_EQ(*result.request_id, 5u);

  const auto response_frame = channel.BuildServiceResponse(
      *result.request_id, std::vector<char>{'o', 'k'});
  const auto response = DecodeSecureConversationMessage(response_frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->frame_header.message_type,
            MessageType::SecureMessage);
  EXPECT_EQ(response->sequence_header.request_id, 5u);
  EXPECT_EQ(response->body, (std::vector<char>{'o', 'k'}));
}

TEST(SecureChannelTest, OpenRequestBodyRoundTrips) {
  const OpenSecureChannelRequest request{
      .request_header = {.request_handle = 123,
                         .return_diagnostics = 0,
                         .audit_entry_id = {},
                         .timeout_hint = 0},
      .client_protocol_version = 0,
      .request_type = SecurityTokenRequestType::Renew,
      .security_mode = MessageSecurityMode::None,
      .client_nonce = scada::ByteString{'a', 'b', 'c', 'd'},
      .requested_lifetime = 90000,
  };
  const auto body = EncodeOpenSecureChannelRequestBody(request);
  const auto decoded = DecodeOpenSecureChannelRequestBody(body);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_header.request_handle, 123u);
  EXPECT_EQ(decoded->request_type,
            SecurityTokenRequestType::Renew);
  EXPECT_EQ(decoded->security_mode, MessageSecurityMode::None);
  EXPECT_EQ(decoded->client_nonce, request.client_nonce);
  EXPECT_EQ(decoded->requested_lifetime, 90000u);
}

TEST(SecureChannelTest, OpenResponseBodyRoundTrips) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 77,
                          .service_result = scada::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = 91,
                         .token_id = 4,
                         .created_at = 1234567,
                         .revised_lifetime = 60000},
      .server_nonce = scada::ByteString{'x', 'y'},
  };
  const auto body = EncodeOpenSecureChannelResponseBody(response);
  const auto decoded = DecodeOpenSecureChannelResponseBody(body);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->response_header.request_handle, 77u);
  EXPECT_TRUE(decoded->response_header.service_result.good());
  EXPECT_EQ(decoded->security_token.channel_id, 91u);
  EXPECT_EQ(decoded->security_token.token_id, 4u);
  EXPECT_EQ(decoded->security_token.created_at, 1234567);
  EXPECT_EQ(decoded->security_token.revised_lifetime, 60000u);
  EXPECT_EQ(decoded->server_nonce, response.server_nonce);
}

TEST(SecureChannelTest, CloseRequestBodyRoundTrips) {
  const CloseSecureChannelRequest request{
      .request_header = {.request_handle = 321},
  };
  const auto body = EncodeCloseSecureChannelRequestBody(request);
  const auto decoded = DecodeCloseSecureChannelRequestBody(body);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_header.request_handle, 321u);
}

// A client can build the same OpenSecureChannel request body that the
// existing server-side SecureChannel already accepts end-to-end
// (wrapped in a SecureOpen frame). This wires up the new client encoder
// against the existing server handler without standing up a transport.
TEST(SecureChannelTest, ClientOpenRequestAcceptedByServerChannel) {
  const OpenSecureChannelRequest client_request{
      .request_header = {.request_handle = 42},
      .client_protocol_version = 0,
      .request_type = SecurityTokenRequestType::Issue,
      .security_mode = MessageSecurityMode::None,
      .client_nonce = {},
      .requested_lifetime = 60000,
  };
  const auto body = EncodeOpenSecureChannelRequestBody(client_request);
  SecureChannel server_channel{123};
  const auto frame = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header = AsymmetricSecurityHeader{
           .security_policy_uri = std::string{kSecurityPolicyNone},
           .sender_certificate = {},
           .receiver_certificate_thumbprint = {},
       },
       .sequence_header = {.sequence_number = 1, .request_id = 7},
       .body = body});

  const auto result = WaitAwaitable(executor_, server_channel.HandleFrame(frame));
  ASSERT_TRUE(result.outbound_frame.has_value());
  EXPECT_FALSE(result.close_transport);
  EXPECT_TRUE(server_channel.opened());

  // The server produces a valid OpenSecureChannelResponse body that the
  // client-side decoder consumes.
  const auto response_message =
      DecodeSecureConversationMessage(*result.outbound_frame);
  ASSERT_TRUE(response_message.has_value());
  const auto response_body =
      DecodeOpenSecureChannelResponseBody(response_message->body);
  ASSERT_TRUE(response_body.has_value());
  EXPECT_EQ(response_body->response_header.request_handle, 42u);
  EXPECT_TRUE(response_body->response_header.service_result.good());
  EXPECT_EQ(response_body->security_token.channel_id, 123u);
  EXPECT_EQ(response_body->security_token.token_id, server_channel.token_id());
}

TEST(SecureChannelTest, CloseRequestClosesTransport) {
  SecureChannel channel{11};
  const auto open_frame = EncodeSecureConversationMessage(
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
  ASSERT_TRUE(WaitAwaitable(executor_, channel.HandleFrame(open_frame))
                  .outbound_frame.has_value());

  const auto close_frame = EncodeSecureConversationMessage(
      {.frame_header =
           {.message_type = MessageType::SecureClose,
            .chunk_type = 'F',
            .message_size = 0},
       .secure_channel_id = 11,
       .symmetric_security_header = SymmetricSecurityHeader{
           .token_id = channel.token_id()},
       .sequence_header = {.sequence_number = 2, .request_id = 2},
       .body = EncodeCloseRequestBody(2)});

  const auto result = WaitAwaitable(executor_, channel.HandleFrame(close_frame));
  EXPECT_TRUE(result.close_transport);
}

TEST(SecureChannelTest,
     DecodeOpenResponseRejectsTruncatedPayload) {
  // A well-formed body wraps the response payload in an ExtensionObject with
  // type_id = kOpenSecureChannelResponseEncodingId. Truncating past
  // the wrapper should yield std::nullopt, not garbage.
  auto body = EncodeOpenSecureChannelResponseBody(
      {.response_header = {.request_handle = 1,
                           .service_result = scada::StatusCode::Good},
       .server_protocol_version = 0,
       .security_token = {.channel_id = 1,
                          .token_id = 1,
                          .created_at = 0,
                          .revised_lifetime = 60000},
       .server_nonce = {}});
  ASSERT_GT(body.size(), 16u);
  body.resize(body.size() - 4);
  const auto decoded = DecodeOpenSecureChannelResponseBody(body);
  EXPECT_FALSE(decoded.has_value());
}

TEST(SecureChannelTest,
     DecodeOpenResponseRejectsWrongExtensionTypeId) {
  // Hand-roll a body whose extension object has a bogus type id and confirm
  // the decoder refuses it rather than treating arbitrary bytes as a
  // response payload.
  std::vector<char> body;
  Encoder encoder{body};
  encoder.Encode(scada::NodeId{/*numeric id*/ 9999});  // wrong type id
  encoder.Encode(std::uint8_t{0x01});                  // ByteString encoding
  encoder.Encode(std::int32_t{0});                     // empty payload
  EXPECT_FALSE(DecodeOpenSecureChannelResponseBody(body).has_value());
}

TEST(SecureChannelTest, OpenResponseRoundTripPreservesBadStatus) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 11,
                          .service_result = scada::StatusCode::Bad},
      .server_protocol_version = 0,
      .security_token = {.channel_id = 5,
                         .token_id = 1,
                         .created_at = 0,
                         .revised_lifetime = 0},
      .server_nonce = {},
  };
  const auto body = EncodeOpenSecureChannelResponseBody(response);
  const auto decoded = DecodeOpenSecureChannelResponseBody(body);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->response_header.request_handle, 11u);
  // The server encoder currently emits Good (0) or a generic bad word —
  // either way the client decoder must see "bad" when the input was bad.
  EXPECT_TRUE(decoded->response_header.service_result.bad());
}

TEST(SecureChannelTest,
     DecodeCloseRequestRejectsWrongExtensionTypeId) {
  std::vector<char> body;
  Encoder encoder{body};
  encoder.Encode(scada::NodeId{/*numeric id*/ 12345});
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(std::int32_t{0});
  EXPECT_FALSE(DecodeCloseSecureChannelRequestBody(body).has_value());
}

}  // namespace
}  // namespace opcua::binary
