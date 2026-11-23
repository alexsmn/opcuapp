#include "opcua/transport/binary/protocol.h"

#include <gtest/gtest.h>

#include <string>

namespace opcua::binary {
namespace {

TEST(ProtocolTest, EncodesAndDecodesHelloMessage) {
  const HelloMessage source{
      .protocol_version = 0,
      .receive_buffer_size = 65536,
      .send_buffer_size = 32768,
      .max_message_size = 1048576,
      .max_chunk_count = 16,
      .endpoint_url = "opc.tcp://localhost:4840",
  };

  const auto encoded = EncodeHelloMessage(source);
  const auto header = DecodeFrameHeader(encoded);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->message_type, MessageType::Hello);
  EXPECT_EQ(header->chunk_type, 'F');
  EXPECT_EQ(header->message_size, encoded.size());

  const auto decoded = DecodeHelloMessage(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->protocol_version, source.protocol_version);
  EXPECT_EQ(decoded->receive_buffer_size, source.receive_buffer_size);
  EXPECT_EQ(decoded->send_buffer_size, source.send_buffer_size);
  EXPECT_EQ(decoded->max_message_size, source.max_message_size);
  EXPECT_EQ(decoded->max_chunk_count, source.max_chunk_count);
  EXPECT_EQ(decoded->endpoint_url, source.endpoint_url);
}

TEST(ProtocolTest, EncodesAndDecodesAcknowledgeMessage) {
  const AcknowledgeMessage source{
      .protocol_version = 0,
      .receive_buffer_size = 32768,
      .send_buffer_size = 16384,
      .max_message_size = 0,
      .max_chunk_count = 0,
  };

  const auto encoded = EncodeAcknowledgeMessage(source);
  const auto decoded = DecodeAcknowledgeMessage(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->protocol_version, source.protocol_version);
  EXPECT_EQ(decoded->receive_buffer_size, source.receive_buffer_size);
  EXPECT_EQ(decoded->send_buffer_size, source.send_buffer_size);
  EXPECT_EQ(decoded->max_message_size, source.max_message_size);
  EXPECT_EQ(decoded->max_chunk_count, source.max_chunk_count);
}

TEST(ProtocolTest, EncodesAndDecodesErrorMessage) {
  const ErrorMessage source{
      .error = opcua::StatusCode::Bad_ProtocolVersionUnsupported,
      .reason = "Unsupported protocol version",
  };

  const auto encoded = EncodeErrorMessage(source);
  const auto decoded = DecodeErrorMessage(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->error.code(), source.error.code());
  EXPECT_EQ(decoded->reason, source.reason);
}

// OPC UA Part 6 §7.1.2.6 ReverseHello Message, Table 77,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/7.1.2.6: the header
// says RHE, then ServerUri and EndpointUrl as length-prefixed Strings.
TEST(ProtocolTest, EncodesAndDecodesReverseHelloMessage) {
  const ReverseHelloMessage source{
      .server_uri = "urn:site:scada:iec104",
      .endpoint_url = "opc.tcp://gw:4844",
  };

  const auto encoded = EncodeReverseHelloMessage(source);
  ASSERT_EQ(encoded.size(), 8u + 4u + source.server_uri.size() + 4u +
                                source.endpoint_url.size());
  EXPECT_EQ(std::string(encoded.begin(), encoded.begin() + 4), "RHEF");
  const auto header = DecodeFrameHeader(encoded);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->message_type, MessageType::ReverseHello);
  EXPECT_EQ(header->message_size, encoded.size());
  // The first String's Int32 length prefix, little-endian.
  EXPECT_EQ(static_cast<unsigned char>(encoded[8]), source.server_uri.size());

  const auto decoded = DecodeReverseHelloMessage(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->server_uri, source.server_uri);
  EXPECT_EQ(decoded->endpoint_url, source.endpoint_url);
}

// "The encoded value shall be less than 4096 bytes" (§7.1.2.6).
TEST(ProtocolTest, ReverseHelloStringsAreBoundedAt4095Bytes) {
  const std::string longest(kMaxReverseHelloStringSize, 'x');
  EXPECT_TRUE(DecodeReverseHelloMessage(
                  EncodeReverseHelloMessage(
                      {.server_uri = longest, .endpoint_url = "opc.tcp://a:1"}))
                  .has_value());
  EXPECT_FALSE(DecodeReverseHelloMessage(
                   EncodeReverseHelloMessage({.server_uri = longest + "x",
                                              .endpoint_url = "opc.tcp://a:1"}))
                   .has_value());
  EXPECT_FALSE(DecodeReverseHelloMessage(
                   EncodeReverseHelloMessage(
                       {.server_uri = "urn:a", .endpoint_url = longest + "x"}))
                   .has_value());
}

TEST(ProtocolTest, RejectsATruncatedReverseHello) {
  auto encoded = EncodeReverseHelloMessage(
      {.server_uri = "urn:a", .endpoint_url = "opc.tcp://a:1"});
  encoded.pop_back();
  EXPECT_FALSE(DecodeReverseHelloMessage(encoded).has_value());
}

TEST(ProtocolTest, RejectsUnknownFrameHeaderType) {
  std::vector<char> bytes = {'X', 'Y', 'Z', 'F', 8, 0, 0, 0};
  EXPECT_FALSE(DecodeFrameHeader(bytes).has_value());
}

TEST(ProtocolTest, NegotiatesBuffersAgainstPeerLimits) {
  const HelloMessage hello{
      .protocol_version = 3,
      .receive_buffer_size = 8192,
      .send_buffer_size = 4096,
      .max_message_size = 65536,
      .max_chunk_count = 32,
      .endpoint_url = "opc.tcp://localhost:4840",
  };
  const TransportLimits server{
      .protocol_version = 0,
      .receive_buffer_size = 16384,
      .send_buffer_size = 2048,
      .max_message_size = 131072,
      .max_chunk_count = 64,
  };

  const auto negotiated = NegotiateHello(hello, server);
  ASSERT_TRUE(negotiated.acknowledge.has_value());
  EXPECT_FALSE(negotiated.error.has_value());
  EXPECT_EQ(negotiated.acknowledge->protocol_version, 0u);
  EXPECT_EQ(negotiated.acknowledge->receive_buffer_size, 4096u);
  EXPECT_EQ(negotiated.acknowledge->send_buffer_size, 2048u);
  EXPECT_EQ(negotiated.acknowledge->max_message_size, 131072u);
  EXPECT_EQ(negotiated.acknowledge->max_chunk_count, 64u);
}

TEST(ProtocolTest, RejectsZeroPeerBuffers) {
  const HelloMessage hello{
      .protocol_version = 0,
      .receive_buffer_size = 0,
      .send_buffer_size = 8192,
      .max_message_size = 0,
      .max_chunk_count = 0,
      .endpoint_url = "opc.tcp://localhost:4840",
  };

  const auto negotiated = NegotiateHello(hello, TransportLimits{});
  ASSERT_TRUE(negotiated.error.has_value());
  EXPECT_FALSE(negotiated.acknowledge.has_value());
}

}  // namespace
}  // namespace opcua::binary
