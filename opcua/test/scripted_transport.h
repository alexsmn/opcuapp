#pragma once

// Shared in-memory transport fake for OPC UA binary client tests. A
// ScriptedState holds the bytes the server "sends" (incoming) and records the
// bytes the client writes, so a test can prime a deterministic sequence of
// framed responses and then assert on the requests the client produced.
//
// Frame-building helpers (HEL/ACK, OpenSecureChannel, and service responses)
// mirror the wire format the binary client expects, so tests can drive a full
// connect/discover/session handshake without a real socket or server.

#include "opcua/message.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/service_codec.h"
#include "transport/transport_factory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace opcua::test {

struct ScriptedState {
  std::deque<std::string> incoming;
  std::vector<std::string> writes;
  bool opened = false;
  bool closed = false;
};

// A message-by-message in-memory transport. Each read() yields the next primed
// incoming chunk; each write() is appended to `writes`.
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

// Hands out a transport per CreateTransport call. With a single state every
// transport shares it; with a list of states (e.g. a discovery connection
// followed by a working session) each connection gets its own script in order,
// falling back to the last state for any extra connections.
class ScriptedTransportFactory final : public transport::TransportFactory {
 public:
  explicit ScriptedTransportFactory(std::shared_ptr<ScriptedState> state)
      : states_{std::move(state)} {}
  explicit ScriptedTransportFactory(
      std::vector<std::shared_ptr<ScriptedState>> states)
      : states_{std::move(states)} {}

  transport::expected<transport::any_transport> CreateTransport(
      const transport::TransportString& /*transport_string*/,
      const transport::executor& executor,
      const transport::log_source& /*log*/) override {
    const std::size_t index =
        next_ < states_.size() ? next_ : states_.size() - 1;
    ++next_;
    return transport::any_transport{
        ScriptedTransport{executor, states_[index]}};
  }

 private:
  std::vector<std::shared_ptr<ScriptedState>> states_;
  std::size_t next_ = 0;
};

inline std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

inline constexpr std::uint32_t kScriptedChannelId = 42;
inline constexpr std::uint32_t kScriptedTokenId = 1;

// A SecurityPolicy=None OpenSecureChannel response granting kScriptedChannelId
// / kScriptedTokenId.
inline std::vector<char> BuildOpenResponseFrame() {
  const opcua::binary::OpenSecureChannelResponse response{
      .response_header = {.request_handle = 1,
                          .service_result = StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = kScriptedChannelId,
                         .token_id = kScriptedTokenId,
                         .created_at = 0,
                         .revised_lifetime = 60000},
      .server_nonce = {},
  };
  const opcua::binary::SecureConversationMessage message{
      .frame_header = {.message_type = opcua::binary::MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kScriptedChannelId,
      .asymmetric_security_header =
          opcua::binary::AsymmetricSecurityHeader{
              .security_policy_uri =
                  std::string{opcua::binary::kSecurityPolicyNone},
              .sender_certificate = {},
              .receiver_certificate_thumbprint = {},
          },
      .sequence_header = {.sequence_number = 1, .request_id = 1},
      .body = opcua::binary::EncodeOpenSecureChannelResponseBody(response),
  };
  return opcua::binary::EncodeSecureConversationMessage(message);
}

// A symmetric SecureMessage frame carrying the given service response body.
inline std::vector<char> BuildServiceResponseFrame(std::uint32_t request_id,
                                                   std::uint32_t request_handle,
                                                   opcua::ResponseBody body) {
  const auto encoded =
      opcua::binary::EncodeServiceResponse(request_handle, std::move(body));
  const opcua::binary::SecureConversationMessage message{
      .frame_header = {.message_type =
                           opcua::binary::MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kScriptedChannelId,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          opcua::binary::SymmetricSecurityHeader{.token_id = kScriptedTokenId},
      .sequence_header = {.sequence_number = request_id + 1,
                          .request_id = request_id},
      .body = encoded.value(),
  };
  return opcua::binary::EncodeSecureConversationMessage(message);
}

// Primes the Acknowledge + OpenSecureChannel responses a fresh connection
// reads during connection.Open().
inline void PrimeConnectAndOpen(const std::shared_ptr<ScriptedState>& state) {
  state->incoming.push_back(AsString(opcua::binary::EncodeAcknowledgeMessage(
      {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
  state->incoming.push_back(AsString(BuildOpenResponseFrame()));
}

// Decodes the service requests the client wrote, dropping framing.
inline std::vector<opcua::RequestBody> DecodeServiceRequests(
    const std::vector<std::string>& writes) {
  std::vector<opcua::RequestBody> requests;
  for (const auto& write : writes) {
    const std::vector<char> bytes{write.begin(), write.end()};
    auto message = opcua::binary::DecodeSecureConversationMessage(bytes);
    if (!message) {
      continue;
    }
    auto request = opcua::binary::DecodeServiceRequest(message->body);
    if (request) {
      requests.push_back(std::move(request->body));
    }
  }
  return requests;
}

}  // namespace opcua::test
