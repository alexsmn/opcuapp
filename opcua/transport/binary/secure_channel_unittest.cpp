#include "opcua/transport/binary/secure_channel.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/transport/binary/codec_utils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string_view>

namespace opcua::binary {
namespace {

opcua::TestExecutor executor_;

std::vector<char> EncodeOpenRequestBody(
    std::uint32_t request_handle,
    SecurityTokenRequestType request_type = SecurityTokenRequestType::Issue,
    MessageSecurityMode security_mode = MessageSecurityMode::None,
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
  // OPC UA Part 6 §5.2.2.9 Table 6: TwoByte NodeId is the encoding byte 0x00
  // followed by a one-byte identifier (a null NodeId is identifier 0).
  auto append_nodeid = [&](std::vector<char>& bytes, std::uint32_t id) {
    if (id <= 0xff) {
      append_u8(bytes, 0x00);
      append_u8(bytes, static_cast<std::uint8_t>(id));
    } else {
      append_u8(bytes, 0x01);
      append_u8(bytes, 0);
      append_u16(bytes, static_cast<std::uint16_t>(id));
    }
  };
  // OPC UA Part 6 §6.7: the chunk body is the TypeId NodeId followed directly
  // by the encoded message — no ExtensionObject encoding byte or length.
  auto append_message = [&](std::vector<char>& bytes, std::uint32_t type_id,
                            const std::vector<char>& payload) {
    append_nodeid(bytes, type_id);
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
  append_message(body, kOpenSecureChannelRequestEncodingId, payload);
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
  // OPC UA Part 6 §5.2.2.9 Table 6: TwoByte NodeId is the encoding byte 0x00
  // followed by a one-byte identifier (a null NodeId is identifier 0).
  auto append_nodeid = [&](std::vector<char>& bytes, std::uint32_t id) {
    if (id <= 0xff) {
      append_u8(bytes, 0x00);
      append_u8(bytes, static_cast<std::uint8_t>(id));
    } else {
      append_u8(bytes, 0x01);
      append_u8(bytes, 0);
      append_u16(bytes, static_cast<std::uint16_t>(id));
    }
  };
  // OPC UA Part 6 §6.7: the chunk body is the TypeId NodeId followed directly
  // by the encoded message — no ExtensionObject encoding byte or length.
  auto append_message = [&](std::vector<char>& bytes, std::uint32_t type_id,
                            const std::vector<char>& payload) {
    append_nodeid(bytes, type_id);
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
  append_message(body, kCloseSecureChannelRequestEncodingId, payload);
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
                           .service_result = opcua::StatusCode::Good},
       .server_protocol_version = 0,
       .security_token = {.channel_id = 91,
                          .token_id = 4,
                          .created_at = 0,
                          .revised_lifetime = 60000},
       .server_nonce = {}});
  EXPECT_FALSE(response_body.empty());
}

TEST(SecureChannelTest, OpenRequestProducesOpenResponseFrame) {
  SecureChannel channel{91};
  const auto frame = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {},
           },
       .sequence_header = {.sequence_number = 1, .request_id = 8},
       .body = EncodeOpenRequestBody(55)});

  const auto result =
      opcua::WaitAwaitable(executor_, channel.HandleFrame(frame));
  ASSERT_TRUE(result.outbound_frame.has_value());
  EXPECT_FALSE(result.close_transport);
  EXPECT_TRUE(channel.opened());

  const auto response = DecodeSecureConversationMessage(*result.outbound_frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->frame_header.message_type, MessageType::SecureOpen);
  EXPECT_EQ(response->secure_channel_id, 91u);
  EXPECT_EQ(response->sequence_header.request_id, 8u);
}

// Interop golden: the exact OPN frame a spec-conforming third-party client
// (e.g. open62541) sends for SecurityPolicy None — TwoByte null NodeIds carry
// their identifier byte (OPC UA Part 6 §5.2.2.9) and the body is the TypeId
// followed directly by the message, with no ExtensionObject wrapper (Part 6
// §6.7). Byte-for-byte, independent of this stack's encoders, so an
// opcuapp-to-opcuapp-only framing regression cannot pass unnoticed again.
TEST(SecureChannelTest, AcceptsSpecEncodedOpenRequestFromThirdPartyClient) {
  const std::string_view policy = kSecurityPolicyNone;
  std::vector<char> frame;
  auto u8 = [&](std::uint8_t v) { frame.push_back(static_cast<char>(v)); };
  auto u32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
      u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
  };
  frame.insert(frame.end(), {'O', 'P', 'N', 'F'});
  u32(0);  // message size, patched below
  u32(0);  // secure channel id
  u32(static_cast<std::uint32_t>(policy.size()));
  frame.insert(frame.end(), policy.begin(), policy.end());
  u32(0xffffffff);  // sender certificate: null ByteString
  u32(0xffffffff);  // receiver certificate thumbprint: null ByteString
  u32(1);           // sequence number
  u32(1);           // request id
  frame.insert(frame.end(),
               {0x01, 0x00, static_cast<char>(0xbe), 0x01});  // TypeId i=446
  u8(0x00);  // authenticationToken: TwoByte null NodeId...
  u8(0x00);  // ...with its one-byte identifier
  for (int i = 0; i < 8; ++i)
    u8(0x00);       // timestamp
  u32(1);           // request handle
  u32(0);           // return diagnostics
  u32(0xffffffff);  // audit entry id: null String
  u32(0);           // timeout hint
  u8(0x00);         // additionalHeader: TwoByte null NodeId...
  u8(0x00);         // ...identifier byte...
  u8(0x00);         // ...and ExtensionObject encoding mask (no body)
  u32(0);           // client protocol version
  u32(0);           // request type: Issue
  u32(1);           // security mode: None
  u32(0xffffffff);  // client nonce: null ByteString
  u32(3600000);     // requested lifetime
  const auto size = static_cast<std::uint32_t>(frame.size());
  std::memcpy(frame.data() + 4, &size, sizeof(size));

  SecureChannel channel{33};
  const auto result =
      opcua::WaitAwaitable(executor_, channel.HandleFrame(frame));
  EXPECT_FALSE(result.close_transport);
  ASSERT_TRUE(result.outbound_frame.has_value());
  EXPECT_TRUE(channel.opened());

  const auto response = DecodeSecureConversationMessage(*result.outbound_frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->frame_header.message_type, MessageType::SecureOpen);
  EXPECT_EQ(response->secure_channel_id, 33u);
  EXPECT_EQ(response->sequence_header.request_id, 1u);
}

TEST(SecureChannelTest, RejectsUnsupportedSecurityModeInOpen) {
  SecureChannel channel{19};
  const auto frame = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {},
           },
       .sequence_header = {.sequence_number = 1, .request_id = 2},
       .body = EncodeOpenRequestBody(12, SecurityTokenRequestType::Issue,
                                     MessageSecurityMode::Sign)});

  const auto result =
      opcua::WaitAwaitable(executor_, channel.HandleFrame(frame));
  ASSERT_TRUE(result.outbound_frame.has_value());
  EXPECT_FALSE(channel.opened());
}

TEST(SecureChannelTest, RoutesMessageBodyAfterOpen) {
  SecureChannel channel{7};
  const auto open_frame = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {},
           },
       .sequence_header = {.sequence_number = 1, .request_id = 4},
       .body = EncodeOpenRequestBody(44)});
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, channel.HandleFrame(open_frame))
                  .outbound_frame.has_value());

  const std::vector<char> payload{'x', 'y', 'z'};
  const auto msg_frame = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureMessage,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 7,
       .symmetric_security_header =
           SymmetricSecurityHeader{.token_id = channel.token_id()},
       .sequence_header = {.sequence_number = 2, .request_id = 5},
       .body = payload});

  const auto result =
      opcua::WaitAwaitable(executor_, channel.HandleFrame(msg_frame));
  ASSERT_TRUE(result.service_payload.has_value());
  EXPECT_EQ(*result.service_payload, payload);
  ASSERT_TRUE(result.request_id.has_value());
  EXPECT_EQ(*result.request_id, 5u);

  const auto response_frame = channel.BuildServiceResponse(
      *result.request_id, std::vector<char>{'o', 'k'});
  const auto response = DecodeSecureConversationMessage(response_frame);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->frame_header.message_type, MessageType::SecureMessage);
  EXPECT_EQ(response->sequence_header.request_id, 5u);
  EXPECT_EQ(response->body, (std::vector<char>{'o', 'k'}));
}

// Regression: the Renew response must advertise the token the server expects
// NEXT (OPC UA Part 4 §5.5.2 ChannelSecurityToken) — it used to encode the
// superseded id (rotation happened after building the response), so a client
// adopting the advertised token was dropped on its next MSG. In deployment
// this killed every SecurityPolicy-None inter-tier session at each renewal
// and surfaced as the aggregating proxy's periodic downstream flaps. The
// previous token also stays accepted during the switchover (Part 6 §6.7.4).
TEST(SecureChannelTest, RenewAdvertisesUsableTokenAndKeepsPreviousValid) {
  SecureChannel channel{21};
  auto open = [&](std::uint32_t request_handle, SecurityTokenRequestType type,
                  std::uint32_t sequence, std::uint32_t request_id) {
    return EncodeSecureConversationMessage(
        {.frame_header = {.message_type = MessageType::SecureOpen,
                          .chunk_type = 'F',
                          .message_size = 0},
         .secure_channel_id = type == SecurityTokenRequestType::Renew ? 21u : 0u,
         .asymmetric_security_header =
             AsymmetricSecurityHeader{
                 .security_policy_uri = std::string{kSecurityPolicyNone},
                 .sender_certificate = {},
                 .receiver_certificate_thumbprint = {},
             },
         .sequence_header = {.sequence_number = sequence,
                             .request_id = request_id},
         .body = EncodeOpenRequestBody(request_handle, type,
                                       MessageSecurityMode::None)});
  };

  ASSERT_TRUE(opcua::WaitAwaitable(
                  executor_,
                  channel.HandleFrame(open(1, SecurityTokenRequestType::Issue,
                                           /*sequence=*/1, /*request_id=*/1)))
                  .outbound_frame.has_value());
  const std::uint32_t issued_token_id = channel.token_id();

  const auto renew_result = opcua::WaitAwaitable(
      executor_, channel.HandleFrame(open(2, SecurityTokenRequestType::Renew,
                                          /*sequence=*/2, /*request_id=*/2)));
  ASSERT_TRUE(renew_result.outbound_frame.has_value());
  EXPECT_FALSE(renew_result.close_transport);

  const auto renew_response =
      DecodeSecureConversationMessage(*renew_result.outbound_frame);
  ASSERT_TRUE(renew_response.has_value());
  const auto renew_body =
      DecodeOpenSecureChannelResponseBody(renew_response->body);
  ASSERT_TRUE(renew_body.has_value());
  // The advertised token is the one the server now expects.
  EXPECT_EQ(renew_body->security_token.token_id, channel.token_id());
  EXPECT_NE(renew_body->security_token.token_id, issued_token_id);

  auto message_with_token = [&](std::uint32_t token_id, std::uint32_t sequence,
                                std::uint32_t request_id) {
    return EncodeSecureConversationMessage(
        {.frame_header = {.message_type = MessageType::SecureMessage,
                          .chunk_type = 'F',
                          .message_size = 0},
         .secure_channel_id = 21,
         .symmetric_security_header =
             SymmetricSecurityHeader{.token_id = token_id},
         .sequence_header = {.sequence_number = sequence,
                             .request_id = request_id},
         .body = std::vector<char>{'m'}});
  };

  // An MSG with the advertised (renewed) token must be routed, not dropped.
  const auto renewed_msg = opcua::WaitAwaitable(
      executor_,
      channel.HandleFrame(message_with_token(
          renew_body->security_token.token_id, /*sequence=*/3,
          /*request_id=*/3)));
  EXPECT_FALSE(renewed_msg.close_transport);
  EXPECT_TRUE(renewed_msg.service_payload.has_value());

  // A straggler still secured with the superseded token stays accepted.
  const auto straggler_msg = opcua::WaitAwaitable(
      executor_, channel.HandleFrame(message_with_token(
                     issued_token_id, /*sequence=*/4, /*request_id=*/4)));
  EXPECT_FALSE(straggler_msg.close_transport);
  EXPECT_TRUE(straggler_msg.service_payload.has_value());

  // A token never issued is still rejected.
  const auto bogus_msg = opcua::WaitAwaitable(
      executor_, channel.HandleFrame(message_with_token(
                     channel.token_id() + 7, /*sequence=*/5,
                     /*request_id=*/5)));
  EXPECT_TRUE(bogus_msg.close_transport);
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
      .client_nonce = opcua::ByteString{'a', 'b', 'c', 'd'},
      .requested_lifetime = 90000,
  };
  const auto body = EncodeOpenSecureChannelRequestBody(request);
  const auto decoded = DecodeOpenSecureChannelRequestBody(body);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_header.request_handle, 123u);
  EXPECT_EQ(decoded->request_type, SecurityTokenRequestType::Renew);
  EXPECT_EQ(decoded->security_mode, MessageSecurityMode::None);
  EXPECT_EQ(decoded->client_nonce, request.client_nonce);
  EXPECT_EQ(decoded->requested_lifetime, 90000u);
}

TEST(SecureChannelTest, OpenResponseBodyRoundTrips) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 77,
                          .service_result = opcua::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = 91,
                         .token_id = 4,
                         .created_at = 1234567,
                         .revised_lifetime = 60000},
      .server_nonce = opcua::ByteString{'x', 'y'},
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
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {},
           },
       .sequence_header = {.sequence_number = 1, .request_id = 7},
       .body = body});

  const auto result =
      opcua::WaitAwaitable(executor_, server_channel.HandleFrame(frame));
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
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {},
           },
       .sequence_header = {.sequence_number = 1, .request_id = 1},
       .body = EncodeOpenRequestBody(1)});
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, channel.HandleFrame(open_frame))
                  .outbound_frame.has_value());

  const auto close_frame = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureClose,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 11,
       .symmetric_security_header =
           SymmetricSecurityHeader{.token_id = channel.token_id()},
       .sequence_header = {.sequence_number = 2, .request_id = 2},
       .body = EncodeCloseRequestBody(2)});

  const auto result =
      opcua::WaitAwaitable(executor_, channel.HandleFrame(close_frame));
  EXPECT_TRUE(result.close_transport);
}

TEST(SecureChannelTest, DecodeOpenResponseRejectsTruncatedPayload) {
  // A well-formed body wraps the response payload in an ExtensionObject with
  // type_id = kOpenSecureChannelResponseEncodingId. Truncating past
  // the wrapper should yield std::nullopt, not garbage.
  auto body = EncodeOpenSecureChannelResponseBody(
      {.response_header = {.request_handle = 1,
                           .service_result = opcua::StatusCode::Good},
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

TEST(SecureChannelTest, DecodeOpenResponseRejectsWrongExtensionTypeId) {
  // Hand-roll a body whose extension object has a bogus type id and confirm
  // the decoder refuses it rather than treating arbitrary bytes as a
  // response payload.
  std::vector<char> body;
  Encoder encoder{body};
  encoder.Encode(opcua::NodeId{/*numeric id*/ 9999});  // wrong type id
  encoder.Encode(std::uint8_t{0x01});                  // ByteString encoding
  encoder.Encode(std::int32_t{0});                     // empty payload
  EXPECT_FALSE(DecodeOpenSecureChannelResponseBody(body).has_value());
}

TEST(SecureChannelTest, OpenResponseRoundTripPreservesBadStatus) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 11,
                          .service_result = opcua::StatusCode::Bad},
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

TEST(SecureChannelTest, DecodeCloseRequestRejectsWrongExtensionTypeId) {
  std::vector<char> body;
  Encoder encoder{body};
  encoder.Encode(opcua::NodeId{/*numeric id*/ 12345});
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(std::int32_t{0});
  EXPECT_FALSE(DecodeCloseSecureChannelRequestBody(body).has_value());
}

}  // namespace
}  // namespace opcua::binary
