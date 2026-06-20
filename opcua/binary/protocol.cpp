#include "opcua/binary/protocol.h"
#include "opcua/binary/codec_utils.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace opcua::binary {
namespace {

constexpr std::size_t kHeaderSize = 8;
constexpr char kFinalChunkType = 'F';

std::array<char, 3> EncodeMessageType(MessageType message_type) {
  switch (message_type) {
    case MessageType::Hello:
      return {'H', 'E', 'L'};
    case MessageType::Acknowledge:
      return {'A', 'C', 'K'};
    case MessageType::Error:
      return {'E', 'R', 'R'};
    case MessageType::ReverseHello:
      return {'R', 'H', 'E'};
    case MessageType::SecureOpen:
      return {'O', 'P', 'N'};
    case MessageType::SecureMessage:
      return {'M', 'S', 'G'};
    case MessageType::SecureClose:
      return {'C', 'L', 'O'};
  }
  return {'E', 'R', 'R'};
}

std::optional<MessageType> DecodeMessageType(
    std::string_view message_type) {
  if (message_type == "HEL") {
    return MessageType::Hello;
  }
  if (message_type == "ACK") {
    return MessageType::Acknowledge;
  }
  if (message_type == "ERR") {
    return MessageType::Error;
  }
  if (message_type == "RHE") {
    return MessageType::ReverseHello;
  }
  if (message_type == "OPN") {
    return MessageType::SecureOpen;
  }
  if (message_type == "MSG") {
    return MessageType::SecureMessage;
  }
  if (message_type == "CLO") {
    return MessageType::SecureClose;
  }
  return std::nullopt;
}

template <typename FillBody>
std::vector<char> EncodeWithHeader(MessageType message_type,
                                   FillBody&& fill_body) {
  std::vector<char> bytes(kHeaderSize, '\0');
  fill_body(bytes);

  const auto encoded_type = EncodeMessageType(message_type);
  bytes[0] = encoded_type[0];
  bytes[1] = encoded_type[1];
  bytes[2] = encoded_type[2];
  bytes[3] = kFinalChunkType;
  const auto message_size = static_cast<std::uint32_t>(bytes.size());
  std::memcpy(bytes.data() + 4, &message_size, sizeof(message_size));
  return bytes;
}

}  // namespace

std::optional<FrameHeader> DecodeFrameHeader(
    const std::vector<char>& bytes) {
  if (bytes.size() < kHeaderSize) {
    return std::nullopt;
  }

  const auto message_type = DecodeMessageType(
      std::string_view{bytes.data(), 3});
  if (!message_type.has_value()) {
    return std::nullopt;
  }

  FrameHeader header{
      .message_type = *message_type,
      .chunk_type = bytes[3],
  };
  std::memcpy(&header.message_size, bytes.data() + 4,
              sizeof(header.message_size));
  if (header.message_size < kHeaderSize) {
    return std::nullopt;
  }
  return header;
}

std::vector<char> EncodeFrameHeader(
    const FrameHeader& header) {
  std::vector<char> bytes(kHeaderSize, '\0');
  const auto message_type = EncodeMessageType(header.message_type);
  bytes[0] = message_type[0];
  bytes[1] = message_type[1];
  bytes[2] = message_type[2];
  bytes[3] = header.chunk_type;
  std::memcpy(bytes.data() + 4, &header.message_size,
              sizeof(header.message_size));
  return bytes;
}

std::vector<char> EncodeHelloMessage(
    const HelloMessage& message) {
  return EncodeWithHeader(MessageType::Hello,
                          [&](std::vector<char>& bytes) {
                            Encoder encoder{bytes};
                            encoder.Encode(message.protocol_version);
                            encoder.Encode(message.receive_buffer_size);
                            encoder.Encode(message.send_buffer_size);
                            encoder.Encode(message.max_message_size);
                            encoder.Encode(message.max_chunk_count);
                            encoder.Encode(message.endpoint_url);
                          });
}

std::optional<HelloMessage> DecodeHelloMessage(
    const std::vector<char>& bytes) {
  const auto header = DecodeFrameHeader(bytes);
  if (!header || header->message_type != MessageType::Hello ||
      header->message_size != bytes.size()) {
    return std::nullopt;
  }

  HelloMessage message;
  Decoder decoder{std::span<const char>{bytes}.subspan(kHeaderSize)};
  if (!decoder.Decode(message.protocol_version) ||
      !decoder.Decode(message.receive_buffer_size) ||
      !decoder.Decode(message.send_buffer_size) ||
      !decoder.Decode(message.max_message_size) ||
      !decoder.Decode(message.max_chunk_count) ||
      !decoder.Decode(message.endpoint_url)) {
    return std::nullopt;
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return message;
}

std::vector<char> EncodeAcknowledgeMessage(
    const AcknowledgeMessage& message) {
  return EncodeWithHeader(MessageType::Acknowledge,
                          [&](std::vector<char>& bytes) {
                            Encoder encoder{bytes};
                            encoder.Encode(message.protocol_version);
                            encoder.Encode(message.receive_buffer_size);
                            encoder.Encode(message.send_buffer_size);
                            encoder.Encode(message.max_message_size);
                            encoder.Encode(message.max_chunk_count);
                          });
}

std::optional<AcknowledgeMessage> DecodeAcknowledgeMessage(
    const std::vector<char>& bytes) {
  const auto header = DecodeFrameHeader(bytes);
  if (!header || header->message_type != MessageType::Acknowledge ||
      header->message_size != bytes.size()) {
    return std::nullopt;
  }

  AcknowledgeMessage message;
  Decoder decoder{std::span<const char>{bytes}.subspan(kHeaderSize)};
  if (!decoder.Decode(message.protocol_version) ||
      !decoder.Decode(message.receive_buffer_size) ||
      !decoder.Decode(message.send_buffer_size) ||
      !decoder.Decode(message.max_message_size) ||
      !decoder.Decode(message.max_chunk_count)) {
    return std::nullopt;
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  return message;
}

std::vector<char> EncodeErrorMessage(
    const ErrorMessage& message) {
  return EncodeWithHeader(MessageType::Error,
                          [&](std::vector<char>& bytes) {
                            Encoder encoder{bytes};
                            encoder.Encode(message.error.full_code());
                            encoder.Encode(message.reason);
                          });
}

std::optional<ErrorMessage> DecodeErrorMessage(
    const std::vector<char>& bytes) {
  const auto header = DecodeFrameHeader(bytes);
  if (!header || header->message_type != MessageType::Error ||
      header->message_size != bytes.size()) {
    return std::nullopt;
  }

  ErrorMessage message;
  std::uint32_t full_code = 0;
  Decoder decoder{std::span<const char>{bytes}.subspan(kHeaderSize)};
  if (!decoder.Decode(full_code) ||
      !decoder.Decode(message.reason)) {
    return std::nullopt;
  }
  if (!decoder.consumed()) {
    return std::nullopt;
  }
  message.error = scada::Status::FromFullCode(full_code);
  return message;
}

NegotiationResult NegotiateHello(
    const HelloMessage& hello,
    const TransportLimits& server_limits) {
  if (hello.receive_buffer_size == 0 || hello.send_buffer_size == 0) {
    return {.error = ErrorMessage{
                .error = scada::StatusCode::Bad,
                .reason = "Client transport buffers must be non-zero",
            }};
  }

  AcknowledgeMessage acknowledge;
  acknowledge.protocol_version = std::min(hello.protocol_version,
                                          server_limits.protocol_version);
  acknowledge.receive_buffer_size =
      std::min(server_limits.receive_buffer_size, hello.send_buffer_size);
  acknowledge.send_buffer_size =
      std::min(server_limits.send_buffer_size, hello.receive_buffer_size);
  acknowledge.max_message_size = server_limits.max_message_size;
  acknowledge.max_chunk_count = server_limits.max_chunk_count;

  return {.acknowledge = acknowledge};
}

}  // namespace opcua::binary
