#pragma once

#include "opcua/types/status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opcua::binary {

enum class MessageType {
  Hello,
  Acknowledge,
  Error,
  ReverseHello,
  SecureOpen,
  SecureMessage,
  SecureClose,
};

struct FrameHeader {
  MessageType message_type = MessageType::Hello;
  char chunk_type = 'F';
  std::uint32_t message_size = 0;
};

struct HelloMessage {
  std::uint32_t protocol_version = 0;
  std::uint32_t receive_buffer_size = 0;
  std::uint32_t send_buffer_size = 0;
  std::uint32_t max_message_size = 0;
  std::uint32_t max_chunk_count = 0;
  std::string endpoint_url;
};

struct AcknowledgeMessage {
  std::uint32_t protocol_version = 0;
  std::uint32_t receive_buffer_size = 0;
  std::uint32_t send_buffer_size = 0;
  std::uint32_t max_message_size = 0;
  std::uint32_t max_chunk_count = 0;
};

struct ErrorMessage {
  Status error = StatusCode::Bad;
  std::string reason;
};

struct TransportLimits {
  std::uint32_t protocol_version = 0;
  std::uint32_t receive_buffer_size = 65535;
  std::uint32_t send_buffer_size = 65535;
  std::uint32_t max_message_size = 0;
  std::uint32_t max_chunk_count = 0;
};

struct NegotiationResult {
  std::optional<AcknowledgeMessage> acknowledge;
  std::optional<ErrorMessage> error;
};

[[nodiscard]] std::optional<FrameHeader> DecodeFrameHeader(
    const std::vector<char>& bytes);
[[nodiscard]] std::vector<char> EncodeFrameHeader(const FrameHeader& header);

[[nodiscard]] std::vector<char> EncodeHelloMessage(const HelloMessage& message);
[[nodiscard]] std::optional<HelloMessage> DecodeHelloMessage(
    const std::vector<char>& bytes);

[[nodiscard]] std::vector<char> EncodeAcknowledgeMessage(
    const AcknowledgeMessage& message);
[[nodiscard]] std::optional<AcknowledgeMessage> DecodeAcknowledgeMessage(
    const std::vector<char>& bytes);

[[nodiscard]] std::vector<char> EncodeErrorMessage(const ErrorMessage& message);
[[nodiscard]] std::optional<ErrorMessage> DecodeErrorMessage(
    const std::vector<char>& bytes);

// OPC UA Part 6 7.1.2.3/7.1.2.4: the server returns the protocol version it
// will use, constrains send/receive buffers to what the peer can handle, and
// otherwise reports transport-level errors before any SecureChannel messages.
[[nodiscard]] NegotiationResult NegotiateHello(
    const HelloMessage& hello,
    const TransportLimits& server_limits);

}  // namespace opcua::binary
