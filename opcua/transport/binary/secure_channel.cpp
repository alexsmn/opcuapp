#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/codec_utils.h"

#include <cstring>
#include <utility>

namespace opcua::binary {
namespace {

constexpr std::size_t kRsaOaepSha1Overhead = 42;
constexpr std::size_t kHmacSha256TagSize = 32;
constexpr std::size_t kAesBlockSize = 16;

// ByteString is std::vector<char>, so one overload covers both.
std::span<const std::uint8_t> ByteSpan(const std::vector<char>& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// Rewrite the frame header's message_size field (bytes 4..7, little-endian) to
// match the actual byte count of the assembled frame.
void FixUpFrameSize(std::vector<char>& frame) {
  const auto size = static_cast<std::uint32_t>(frame.size());
  std::memcpy(frame.data() + 4, &size, sizeof(size));
}

void AppendNumericNodeId(Encoder& encoder, std::uint32_t id) {
  encoder.Encode(NodeId{id});
}

bool ReadNumericNodeId(Decoder& decoder, std::uint32_t& id) {
  NodeId node_id;
  if (!decoder.Decode(node_id) || !node_id.is_numeric() ||
      node_id.namespace_index() != 0) {
    return false;
  }
  id = node_id.numeric_id();
  return true;
}

void AppendExtensionObject(Encoder& encoder,
                           std::uint32_t type_id,
                           const std::vector<char>& body) {
  encoder.Encode(EncodedExtensionObject{.type_id = type_id, .body = body});
}

bool ReadExtensionObject(Decoder& decoder,
                         std::uint32_t& type_id,
                         std::uint8_t& encoding,
                         std::vector<char>& body) {
  DecodedExtensionObject value;
  if (!decoder.Decode(value)) {
    return false;
  }
  type_id = value.type_id;
  encoding = value.encoding;
  body = std::move(value.body);
  return true;
}

bool ReadRequestHeader(Decoder& decoder, RequestHeader& header) {
  std::uint32_t ignored_node_id = 0;
  std::int64_t ignored_timestamp = 0;
  if (!ReadNumericNodeId(decoder, ignored_node_id) ||
      !decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(header.request_handle) ||
      !decoder.Decode(header.return_diagnostics) ||
      !decoder.Decode(header.audit_entry_id) ||
      !decoder.Decode(header.timeout_hint)) {
    return false;
  }

  std::uint32_t additional_type_id = 0;
  std::uint8_t additional_encoding = 0;
  std::vector<char> additional_body;
  return ReadExtensionObject(decoder, additional_type_id, additional_encoding,
                             additional_body);
}

void AppendResponseHeader(Encoder& encoder, const ResponseHeader& header) {
  encoder.Encode(std::int64_t{0});
  encoder.Encode(header.request_handle);
  encoder.Encode(header.service_result.good() ? 0u : 0x80000000u);
  encoder.Encode(std::uint8_t{0});
  encoder.Encode(std::int32_t{0});
  AppendNumericNodeId(encoder, 0);
  encoder.Encode(std::uint8_t{0x00});
}

void AppendRequestHeader(Encoder& encoder, const RequestHeader& header) {
  AppendNumericNodeId(encoder, 0);
  encoder.Encode(std::int64_t{0});
  encoder.Encode(header.request_handle);
  encoder.Encode(header.return_diagnostics);
  encoder.Encode(header.audit_entry_id);
  encoder.Encode(header.timeout_hint);
  AppendNumericNodeId(encoder, 0);
  encoder.Encode(std::uint8_t{0x00});
}

bool ReadResponseHeader(Decoder& decoder, ResponseHeader& header) {
  std::int64_t ignored_timestamp = 0;
  std::uint32_t status_word = 0;
  std::uint8_t ignored_service_diagnostics_mask = 0;
  std::int32_t ignored_string_table_count = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(header.request_handle) || !decoder.Decode(status_word) ||
      !decoder.Decode(ignored_service_diagnostics_mask) ||
      !decoder.Decode(ignored_string_table_count)) {
    return false;
  }
  header.service_result = Status::FromFullCode(status_word);

  std::uint32_t additional_type_id = 0;
  std::uint8_t additional_encoding = 0;
  std::vector<char> additional_body;
  return ReadExtensionObject(decoder, additional_type_id, additional_encoding,
                             additional_body);
}

}  // namespace

std::optional<SecureConversationMessage> DecodeSecureConversationMessage(
    const std::vector<char>& frame) {
  const auto frame_header = DecodeFrameHeader(frame);
  if (!frame_header || frame_header->message_size != frame.size()) {
    return std::nullopt;
  }

  SecureConversationMessage message;
  message.frame_header = *frame_header;
  Decoder decoder{std::span<const char>{frame}.subspan(8)};
  if (!decoder.Decode(message.secure_channel_id)) {
    return std::nullopt;
  }

  if (frame_header->message_type == MessageType::SecureOpen) {
    AsymmetricSecurityHeader header;
    if (!decoder.Decode(header.security_policy_uri) ||
        !decoder.Decode(header.sender_certificate) ||
        !decoder.Decode(header.receiver_certificate_thumbprint)) {
      return std::nullopt;
    }
    message.asymmetric_security_header = std::move(header);
  } else {
    SymmetricSecurityHeader header;
    if (!decoder.Decode(header.token_id)) {
      return std::nullopt;
    }
    message.symmetric_security_header = header;
  }

  if (!decoder.Decode(message.sequence_header.sequence_number) ||
      !decoder.Decode(message.sequence_header.request_id)) {
    return std::nullopt;
  }

  message.body.assign(
      frame.begin() + static_cast<std::ptrdiff_t>(8 + decoder.offset()),
      frame.end());
  return message;
}

std::vector<char> EncodeSecureConversationMessage(
    const SecureConversationMessage& message) {
  std::vector<char> frame =
      EncodeFrameHeader({.message_type = message.frame_header.message_type,
                         .chunk_type = message.frame_header.chunk_type,
                         .message_size = 0});
  Encoder encoder{frame};
  encoder.Encode(message.secure_channel_id);

  if (message.frame_header.message_type == MessageType::SecureOpen) {
    const auto& header = *message.asymmetric_security_header;
    encoder.Encode(header.security_policy_uri);
    encoder.Encode(header.sender_certificate);
    encoder.Encode(header.receiver_certificate_thumbprint);
  } else {
    encoder.Encode(message.symmetric_security_header->token_id);
  }

  encoder.Encode(message.sequence_header.sequence_number);
  encoder.Encode(message.sequence_header.request_id);
  frame.insert(frame.end(), message.body.begin(), message.body.end());

  const auto message_size = static_cast<std::uint32_t>(frame.size());
  std::memcpy(frame.data() + 4, &message_size, sizeof(message_size));
  return frame;
}

std::optional<OpenSecureChannelRequest> DecodeOpenSecureChannelRequestBody(
    const std::vector<char>& body) {
  Decoder body_decoder{body};
  std::uint32_t type_id = 0;
  std::uint8_t encoding = 0;
  std::vector<char> payload;
  if (!ReadExtensionObject(body_decoder, type_id, encoding, payload) ||
      type_id != kOpenSecureChannelRequestEncodingId || encoding != 0x01 ||
      !body_decoder.consumed()) {
    return std::nullopt;
  }

  OpenSecureChannelRequest request;
  Decoder payload_decoder{payload};
  std::uint32_t request_type = 0;
  std::uint32_t security_mode = 0;
  if (!ReadRequestHeader(payload_decoder, request.request_header) ||
      !payload_decoder.Decode(request.client_protocol_version) ||
      !payload_decoder.Decode(request_type) ||
      !payload_decoder.Decode(security_mode) ||
      !payload_decoder.Decode(request.client_nonce) ||
      !payload_decoder.Decode(request.requested_lifetime) ||
      !payload_decoder.consumed()) {
    return std::nullopt;
  }

  request.request_type = static_cast<SecurityTokenRequestType>(request_type);
  request.security_mode = static_cast<MessageSecurityMode>(security_mode);
  return request;
}

std::vector<char> EncodeOpenSecureChannelResponseBody(
    const OpenSecureChannelResponse& response) {
  std::vector<char> payload;
  Encoder payload_encoder{payload};
  AppendResponseHeader(payload_encoder, response.response_header);
  payload_encoder.Encode(response.server_protocol_version);
  payload_encoder.Encode(response.security_token.channel_id);
  payload_encoder.Encode(response.security_token.token_id);
  payload_encoder.Encode(response.security_token.created_at);
  payload_encoder.Encode(response.security_token.revised_lifetime);
  payload_encoder.Encode(response.server_nonce);

  std::vector<char> body;
  Encoder body_encoder{body};
  AppendExtensionObject(body_encoder, kOpenSecureChannelResponseEncodingId,
                        payload);
  return body;
}

std::optional<CloseSecureChannelRequest> DecodeCloseSecureChannelRequestBody(
    const std::vector<char>& body) {
  Decoder body_decoder{body};
  std::uint32_t type_id = 0;
  std::uint8_t encoding = 0;
  std::vector<char> payload;
  if (!ReadExtensionObject(body_decoder, type_id, encoding, payload) ||
      type_id != kCloseSecureChannelRequestEncodingId || encoding != 0x01 ||
      !body_decoder.consumed()) {
    return std::nullopt;
  }

  CloseSecureChannelRequest request;
  Decoder payload_decoder{payload};
  if (!ReadRequestHeader(payload_decoder, request.request_header) ||
      !payload_decoder.consumed()) {
    return std::nullopt;
  }
  return request;
}

std::vector<char> EncodeOpenSecureChannelRequestBody(
    const OpenSecureChannelRequest& request) {
  std::vector<char> payload;
  Encoder payload_encoder{payload};
  AppendRequestHeader(payload_encoder, request.request_header);
  payload_encoder.Encode(request.client_protocol_version);
  payload_encoder.Encode(static_cast<std::uint32_t>(request.request_type));
  payload_encoder.Encode(static_cast<std::uint32_t>(request.security_mode));
  payload_encoder.Encode(request.client_nonce);
  payload_encoder.Encode(request.requested_lifetime);

  std::vector<char> body;
  Encoder body_encoder{body};
  AppendExtensionObject(body_encoder, kOpenSecureChannelRequestEncodingId,
                        payload);
  return body;
}

std::optional<OpenSecureChannelResponse> DecodeOpenSecureChannelResponseBody(
    const std::vector<char>& body) {
  Decoder body_decoder{body};
  std::uint32_t type_id = 0;
  std::uint8_t encoding = 0;
  std::vector<char> payload;
  if (!ReadExtensionObject(body_decoder, type_id, encoding, payload) ||
      type_id != kOpenSecureChannelResponseEncodingId || encoding != 0x01 ||
      !body_decoder.consumed()) {
    return std::nullopt;
  }

  OpenSecureChannelResponse response;
  Decoder payload_decoder{payload};
  if (!ReadResponseHeader(payload_decoder, response.response_header) ||
      !payload_decoder.Decode(response.server_protocol_version) ||
      !payload_decoder.Decode(response.security_token.channel_id) ||
      !payload_decoder.Decode(response.security_token.token_id) ||
      !payload_decoder.Decode(response.security_token.created_at) ||
      !payload_decoder.Decode(response.security_token.revised_lifetime) ||
      !payload_decoder.Decode(response.server_nonce) ||
      !payload_decoder.consumed()) {
    return std::nullopt;
  }
  return response;
}

std::vector<char> EncodeCloseSecureChannelRequestBody(
    const CloseSecureChannelRequest& request) {
  std::vector<char> payload;
  Encoder payload_encoder{payload};
  AppendRequestHeader(payload_encoder, request.request_header);

  std::vector<char> body;
  Encoder body_encoder{body};
  AppendExtensionObject(body_encoder, kCloseSecureChannelRequestEncodingId,
                        payload);
  return body;
}

StatusOr<std::shared_ptr<const SecureChannelServerConfig>>
MakeSecureChannelServerConfig(
    crypto::Certificate certificate,
    crypto::PrivateKey private_key,
    bool allow_none,
    std::function<Status(std::span<const std::uint8_t>)>
        validate_client_certificate) {
  auto config = std::make_shared<SecureChannelServerConfig>();
  config->allow_none = allow_none;
  config->validate_client_certificate = std::move(validate_client_certificate);

  if (!certificate.empty()) {
    auto der = crypto::CertificateDer(certificate);
    if (!der.ok()) {
      return StatusOr<std::shared_ptr<const SecureChannelServerConfig>>{
          der.status()};
    }
    auto thumbprint = crypto::CertificateThumbprint(certificate);
    if (!thumbprint.ok()) {
      return StatusOr<std::shared_ptr<const SecureChannelServerConfig>>{
          thumbprint.status()};
    }
    config->certificate_der = std::move(*der);
    config->certificate_thumbprint = std::move(*thumbprint);
    config->allow_basic256sha256 = !private_key.empty();
  }
  config->certificate = std::move(certificate);
  config->private_key = std::move(private_key);
  return StatusOr<std::shared_ptr<const SecureChannelServerConfig>>{
      std::shared_ptr<const SecureChannelServerConfig>{std::move(config)}};
}

SecureChannel::SecureChannel(std::uint32_t channel_id)
    : channel_id_{channel_id} {}

SecureChannel::SecureChannel(
    std::shared_ptr<const SecureChannelServerConfig> config,
    std::uint32_t channel_id)
    : config_{std::move(config)}, channel_id_{channel_id} {}

Awaitable<SecureChannel::Result> SecureChannel::HandleFrame(
    std::vector<char> frame) {
  const auto frame_header = DecodeFrameHeader(frame);
  if (!frame_header || frame_header->message_size != frame.size()) {
    co_return Result{.close_transport = true};
  }

  switch (frame_header->message_type) {
    case MessageType::SecureOpen: {
      // Peek the asymmetric security policy URI without decrypting: it precedes
      // the (possibly encrypted) body in the clear (OPC UA Part 6 §6.7.2).
      Decoder dec{std::span<const char>{frame}.subspan(8)};
      std::uint32_t requested_channel_id = 0;
      std::string policy_uri;
      if (!dec.Decode(requested_channel_id) || !dec.Decode(policy_uri)) {
        co_return Result{.close_transport = true};
      }
      if (policy_uri == kSecurityPolicyNone) {
        if (config_ && !config_->allow_none) {
          co_return Result{.close_transport = true};
        }
        co_return HandleOpenNone(frame);
      }
      if (policy_uri == kSecurityPolicyBasic256Sha256 && config_ &&
          config_->allow_basic256sha256 && !config_->certificate_der.empty()) {
        co_return HandleOpenSecure(frame);
      }
      co_return Result{.close_transport = true};
    }

    case MessageType::SecureMessage:
      co_return HandleSecureMessage(frame, /*is_close=*/false);

    case MessageType::SecureClose:
      co_return HandleSecureMessage(frame, /*is_close=*/true);

    case MessageType::Hello:
    case MessageType::Acknowledge:
    case MessageType::Error:
    case MessageType::ReverseHello:
      co_return Result{.close_transport = true};
  }

  co_return Result{.close_transport = true};
}

SecureChannel::Result SecureChannel::HandleOpenNone(
    const std::vector<char>& frame) {
  const auto message = DecodeSecureConversationMessage(frame);
  if (!message.has_value()) {
    return Result{.close_transport = true};
  }
  const auto request = DecodeOpenSecureChannelRequestBody(message->body);
  if (!request.has_value()) {
    return Result{.close_transport = true};
  }

  const auto supported_security =
      request->security_mode == MessageSecurityMode::None &&
      message->asymmetric_security_header &&
      message->asymmetric_security_header->security_policy_uri ==
          kSecurityPolicyNone;

  auto response = BuildOpenResponse(
      *message, *request,
      supported_security ? StatusCode::Good : StatusCode::Bad);
  if (supported_security) {
    opened_ = true;
    if (request->request_type == SecurityTokenRequestType::Renew) {
      ++token_id_;
    }
  }
  return Result{.outbound_frame = std::move(response)};
}

SecureChannel::Result SecureChannel::HandleOpenSecure(
    const std::vector<char>& frame) {
  // Parse the cleartext prefix: channel id + asymmetric security header
  // (policy URI, sender certificate = client cert DER, receiver thumbprint).
  Decoder dec{std::span<const char>{frame}.subspan(8)};
  std::uint32_t requested_channel_id = 0;
  AsymmetricSecurityHeader header;
  if (!dec.Decode(requested_channel_id) ||
      !dec.Decode(header.security_policy_uri) ||
      !dec.Decode(header.sender_certificate) ||
      !dec.Decode(header.receiver_certificate_thumbprint)) {
    return Result{.close_transport = true};
  }
  const std::size_t header_end = 8 + dec.offset();
  if (header_end >= frame.size()) {
    return Result{.close_transport = true};
  }

  // The client must address this server: the receiver thumbprint has to match
  // our own certificate (OPC UA Part 6 §6.7.2).
  if (header.receiver_certificate_thumbprint !=
      config_->certificate_thumbprint) {
    return Result{.close_transport = true};
  }

  // Validate and load the client application instance certificate.
  if (header.sender_certificate.empty()) {
    return Result{.close_transport = true};
  }
  if (config_->validate_client_certificate) {
    const auto validation = config_->validate_client_certificate(
        ByteSpan(header.sender_certificate));
    if (validation.bad()) {
      return Result{.close_transport = true};
    }
  }
  auto client_cert =
      crypto::LoadDerCertificate(ByteSpan(header.sender_certificate));
  if (!client_cert.ok()) {
    return Result{.close_transport = true};
  }
  auto client_public_key = crypto::CertificatePublicKey(*client_cert);
  if (!client_public_key.ok()) {
    return Result{.close_transport = true};
  }

  // Decrypt the ciphertext with our private key.
  std::span<const char> cipher_span{frame.data() + header_end,
                                    frame.size() - header_end};
  auto plaintext = crypto::RsaOaepDecrypt(
      config_->private_key,
      {reinterpret_cast<const std::uint8_t*>(cipher_span.data()),
       cipher_span.size()});
  if (!plaintext.ok()) {
    return Result{.close_transport = true};
  }

  // Verify the client signature over [prefix][plaintext minus signature].
  const std::size_t signature_size =
      static_cast<std::size_t>(client_public_key->KeySizeBytes());
  if (plaintext->size() < signature_size + 1) {
    return Result{.close_transport = true};
  }
  const auto sig_begin = plaintext->size() - signature_size;
  std::vector<char> signed_region;
  signed_region.reserve(header_end + sig_begin);
  signed_region.insert(signed_region.end(), frame.begin(),
                       frame.begin() + static_cast<std::ptrdiff_t>(header_end));
  signed_region.insert(
      signed_region.end(), plaintext->begin(),
      plaintext->begin() + static_cast<std::ptrdiff_t>(sig_begin));
  std::span<const std::uint8_t> signature_bytes{
      reinterpret_cast<const std::uint8_t*>(plaintext->data() + sig_begin),
      signature_size};
  if (!crypto::RsaPkcs1Sha256Verify(*client_public_key, ByteSpan(signed_region),
                                    signature_bytes)) {
    return Result{.close_transport = true};
  }

  // Strip padding, then split into sequence header (8 bytes) and body.
  const auto pad_size = static_cast<std::uint8_t>((*plaintext)[sig_begin - 1]);
  if (sig_begin < static_cast<std::size_t>(1 + pad_size) + 8) {
    return Result{.close_transport = true};
  }
  const auto body_end = sig_begin - 1 - pad_size;
  SequenceHeader sequence_header;
  Decoder seq_dec{std::span<const char>{plaintext->data(), 8}};
  if (!seq_dec.Decode(sequence_header.sequence_number) ||
      !seq_dec.Decode(sequence_header.request_id)) {
    return Result{.close_transport = true};
  }
  const std::vector<char> body{
      plaintext->begin() + 8,
      plaintext->begin() + static_cast<std::ptrdiff_t>(body_end)};

  const auto request = DecodeOpenSecureChannelRequestBody(body);
  if (!request.has_value() ||
      request->security_mode != MessageSecurityMode::SignAndEncrypt) {
    return Result{.close_transport = true};
  }

  // Generate the server nonce and derive the symmetric keys. inbound keys
  // verify/decrypt client traffic (P_SHA256(serverNonce, clientNonce));
  // outbound keys sign/encrypt server traffic (P_SHA256(clientNonce,
  // serverNonce)). OPC UA Part 6 §6.7.5.
  ByteString server_nonce;
  if (config_->server_nonce_generator) {
    auto generated = config_->server_nonce_generator();
    if (!generated.ok() || generated->size() != 32) {
      return Result{.close_transport = true};
    }
    server_nonce = std::move(*generated);
  } else {
    auto generated = crypto::GenerateNonce(32);
    if (!generated.ok()) {
      return Result{.close_transport = true};
    }
    server_nonce = std::move(*generated);
  }
  if (request->client_nonce.size() != 32) {
    return Result{.close_transport = true};
  }

  auto client_thumbprint = crypto::CertificateThumbprint(*client_cert);
  if (!client_thumbprint.ok()) {
    return Result{.close_transport = true};
  }

  if (request->request_type == SecurityTokenRequestType::Renew) {
    ++token_id_;
  }

  auto response = BuildSecureOpenResponse(*request, sequence_header.request_id,
                                          *client_public_key,
                                          *client_thumbprint, server_nonce);
  if (!response.ok()) {
    return Result{.close_transport = true};
  }

  inbound_keys_ = crypto::DeriveBasic256Sha256Keys(
      ByteSpan(server_nonce), ByteSpan(request->client_nonce));
  outbound_keys_ = crypto::DeriveBasic256Sha256Keys(
      ByteSpan(request->client_nonce), ByteSpan(server_nonce));
  server_nonce_ = std::move(server_nonce);
  client_certificate_der_ = std::move(header.sender_certificate);
  basic256_active_ = true;
  opened_ = true;
  return Result{.outbound_frame = std::move(*response)};
}

SecureChannel::Result SecureChannel::HandleSecureMessage(
    const std::vector<char>& frame,
    bool is_close) {
  if (!opened_) {
    return Result{.close_transport = true};
  }

  if (!basic256_active_) {
    const auto message = DecodeSecureConversationMessage(frame);
    if (!message.has_value() || message->secure_channel_id != channel_id_ ||
        !message->symmetric_security_header ||
        message->symmetric_security_header->token_id != token_id_) {
      return Result{.close_transport = true};
    }
    if (is_close) {
      const auto request = DecodeCloseSecureChannelRequestBody(message->body);
      opened_ = false;
      return Result{.close_transport = !request.has_value() || true};
    }
    return Result{.service_payload = message->body,
                  .request_id = message->sequence_header.request_id};
  }

  // Symmetric SignAndEncrypt: header is 16 bytes (type + size + channel id +
  // token id); the remainder is AES-256-CBC ciphertext ending in an
  // HMAC-SHA256 tag (OPC UA Part 6 §6.7.3).
  constexpr std::size_t kHeaderSize = 16;
  if (frame.size() < kHeaderSize) {
    return Result{.close_transport = true};
  }
  std::uint32_t channel_id = 0;
  std::uint32_t token_id = 0;
  std::memcpy(&channel_id, frame.data() + 8, 4);
  std::memcpy(&token_id, frame.data() + 12, 4);
  if (channel_id != channel_id_ || token_id != token_id_) {
    return Result{.close_transport = true};
  }

  std::span<const char> cipher_span{frame.data() + kHeaderSize,
                                    frame.size() - kHeaderSize};
  auto decrypted = crypto::AesCbcDecrypt(
      ByteSpan(inbound_keys_.encrypting_key),
      ByteSpan(inbound_keys_.initialization_vector),
      {reinterpret_cast<const std::uint8_t*>(cipher_span.data()),
       cipher_span.size()});
  if (!decrypted.ok() || decrypted->size() < kHmacSha256TagSize) {
    return Result{.close_transport = true};
  }

  const auto sig_begin = decrypted->size() - kHmacSha256TagSize;
  std::vector<char> signed_region;
  signed_region.reserve(kHeaderSize + sig_begin);
  signed_region.insert(
      signed_region.end(), frame.begin(),
      frame.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
  signed_region.insert(
      signed_region.end(), decrypted->begin(),
      decrypted->begin() + static_cast<std::ptrdiff_t>(sig_begin));
  const auto expected_tag = crypto::HmacSha256(
      ByteSpan(inbound_keys_.signing_key), ByteSpan(signed_region));
  if (expected_tag.size() != kHmacSha256TagSize ||
      std::memcmp(expected_tag.data(), decrypted->data() + sig_begin,
                  kHmacSha256TagSize) != 0) {
    return Result{.close_transport = true};
  }

  const auto pad_size = static_cast<std::uint8_t>((*decrypted)[sig_begin - 1]);
  if (sig_begin < static_cast<std::size_t>(1 + pad_size) + 8) {
    return Result{.close_transport = true};
  }
  const auto body_end = sig_begin - 1 - pad_size;
  std::uint32_t request_id = 0;
  std::memcpy(&request_id, decrypted->data() + 4, 4);
  std::vector<char> body{
      decrypted->begin() + 8,
      decrypted->begin() + static_cast<std::ptrdiff_t>(body_end)};

  if (is_close) {
    const auto request = DecodeCloseSecureChannelRequestBody(body);
    opened_ = false;
    return Result{.close_transport = !request.has_value() || true};
  }
  return Result{.service_payload = std::move(body), .request_id = request_id};
}

std::vector<char> SecureChannel::BuildServiceResponse(std::uint32_t request_id,
                                                      std::vector<char> body) {
  if (basic256_active_) {
    auto framed = BuildSecureServiceResponse(request_id, body);
    return framed.ok() ? std::move(*framed) : std::vector<char>{};
  }

  SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id_,
      .symmetric_security_header =
          SymmetricSecurityHeader{.token_id = token_id_},
      .sequence_header = {.sequence_number = next_sequence_number_++,
                          .request_id = request_id},
      .body = std::move(body),
  };
  return EncodeSecureConversationMessage(message);
}

std::vector<char> SecureChannel::BuildOpenResponse(
    const SecureConversationMessage& request_message,
    const OpenSecureChannelRequest& request,
    Status service_result) {
  OpenSecureChannelResponse response{
      .response_header = {.request_handle =
                              request.request_header.request_handle,
                          .service_result = service_result},
      .server_protocol_version = request.client_protocol_version,
      .security_token = {.channel_id = channel_id_,
                         .token_id = token_id_,
                         .created_at = 0,
                         .revised_lifetime = request.requested_lifetime},
      .server_nonce = {},
  };

  SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id_,
      .asymmetric_security_header =
          AsymmetricSecurityHeader{
              .security_policy_uri = std::string{kSecurityPolicyNone},
              .sender_certificate = {},
              .receiver_certificate_thumbprint = {},
          },
      .sequence_header = {.sequence_number = next_sequence_number_++,
                          .request_id =
                              request_message.sequence_header.request_id},
      .body = EncodeOpenSecureChannelResponseBody(response),
  };
  return EncodeSecureConversationMessage(message);
}

StatusOr<std::vector<char>> SecureChannel::BuildSecureOpenResponse(
    const OpenSecureChannelRequest& request,
    std::uint32_t request_id,
    const crypto::PrivateKey& client_public_key,
    const ByteString& client_certificate_thumbprint,
    const ByteString& server_nonce) {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle =
                              request.request_header.request_handle,
                          .service_result = StatusCode::Good},
      .server_protocol_version = request.client_protocol_version,
      .security_token = {.channel_id = channel_id_,
                         .token_id = token_id_,
                         .created_at = 0,
                         .revised_lifetime = request.requested_lifetime},
      .server_nonce = server_nonce,
  };
  const auto body = EncodeOpenSecureChannelResponseBody(response);

  // Cleartext prefix: frame header + channel id + asymmetric security header
  // (policy URI, our certificate, the client's certificate thumbprint).
  std::vector<char> prefix;
  prefix.insert(prefix.end(), {'O', 'P', 'N', 'F', 0, 0, 0, 0});
  {
    Encoder enc{prefix};
    enc.Encode(channel_id_);
    enc.Encode(std::string{kSecurityPolicyBasic256Sha256});
    enc.Encode(config_->certificate_der);
    enc.Encode(client_certificate_thumbprint);
  }

  // Plaintext to encrypt: [seq header][body][padding][PaddingSize][signature].
  // Encryption uses the client's public key (block = clientKeyBytes - 42);
  // the signature is RSA-PKCS1-SHA256 with our private key (clientKeyBytes
  // not relevant — signature_size is our key size).
  std::vector<char> to_encrypt;
  {
    Encoder enc{to_encrypt};
    enc.Encode(next_sequence_number_++);
    enc.Encode(request_id);
  }
  to_encrypt.insert(to_encrypt.end(), body.begin(), body.end());

  const std::size_t client_key_bytes =
      static_cast<std::size_t>(client_public_key.KeySizeBytes());
  const std::size_t server_key_bytes =
      static_cast<std::size_t>(config_->private_key.KeySizeBytes());
  if (client_key_bytes <= kRsaOaepSha1Overhead || server_key_bytes == 0) {
    return StatusOr<std::vector<char>>{Status{StatusCode::Bad}};
  }
  const std::size_t plain_block = client_key_bytes - kRsaOaepSha1Overhead;
  const std::size_t signature_size = server_key_bytes;

  const std::size_t unpadded = to_encrypt.size() + 1 + signature_size;
  const std::size_t pad_count =
      (plain_block - unpadded % plain_block) % plain_block;
  if (pad_count > 255) {
    return StatusOr<std::vector<char>>{Status{StatusCode::Bad}};
  }
  to_encrypt.insert(to_encrypt.end(), pad_count, static_cast<char>(pad_count));
  to_encrypt.push_back(static_cast<char>(pad_count));

  // Stamp the final frame size into the prefix before signing so the signature
  // covers the chunk header (OPC UA Part 6 §6.7.2).
  const std::size_t total_plaintext = to_encrypt.size() + signature_size;
  const std::size_t num_blocks = total_plaintext / plain_block;
  const std::size_t cipher_size = num_blocks * client_key_bytes;
  const std::size_t predicted_final_size = prefix.size() + cipher_size;
  {
    const auto size32 = static_cast<std::uint32_t>(predicted_final_size);
    std::memcpy(prefix.data() + 4, &size32, sizeof(size32));
  }

  std::vector<char> to_sign;
  to_sign.reserve(prefix.size() + to_encrypt.size());
  to_sign.insert(to_sign.end(), prefix.begin(), prefix.end());
  to_sign.insert(to_sign.end(), to_encrypt.begin(), to_encrypt.end());
  auto signature =
      crypto::RsaPkcs1Sha256Sign(config_->private_key, ByteSpan(to_sign));
  if (!signature.ok()) {
    return StatusOr<std::vector<char>>{signature.status()};
  }
  to_encrypt.insert(to_encrypt.end(), signature->begin(), signature->end());

  auto ciphertext =
      crypto::RsaOaepEncrypt(client_public_key, ByteSpan(to_encrypt));
  if (!ciphertext.ok()) {
    return StatusOr<std::vector<char>>{ciphertext.status()};
  }

  std::vector<char> final_frame;
  final_frame.reserve(prefix.size() + ciphertext->size());
  final_frame.insert(final_frame.end(), prefix.begin(), prefix.end());
  final_frame.insert(final_frame.end(), ciphertext->begin(), ciphertext->end());
  FixUpFrameSize(final_frame);
  return StatusOr<std::vector<char>>{std::move(final_frame)};
}

StatusOr<std::vector<char>> SecureChannel::BuildSecureServiceResponse(
    std::uint32_t request_id,
    const std::vector<char>& body) {
  // Plaintext: [seq header][body][padding][PaddingSize], padded so the whole
  // (payload + 1 + 32-byte HMAC tag) is a multiple of the AES block size.
  std::vector<char> plaintext_payload;
  {
    Encoder enc{plaintext_payload};
    enc.Encode(next_sequence_number_++);
    enc.Encode(request_id);
  }
  plaintext_payload.insert(plaintext_payload.end(), body.begin(), body.end());

  const std::size_t pad_count =
      (kAesBlockSize -
       (plaintext_payload.size() + 1 + kHmacSha256TagSize) % kAesBlockSize) %
      kAesBlockSize;
  if (pad_count > 255) {
    return StatusOr<std::vector<char>>{Status{StatusCode::Bad}};
  }
  plaintext_payload.insert(plaintext_payload.end(), pad_count,
                           static_cast<char>(pad_count));
  plaintext_payload.push_back(static_cast<char>(pad_count));

  // 16-byte header: "MSGF" + size + channel id + token id.
  std::vector<char> header_portion;
  header_portion.insert(header_portion.end(), {'M', 'S', 'G', 'F', 0, 0, 0, 0});
  {
    Encoder enc{header_portion};
    enc.Encode(channel_id_);
    enc.Encode(token_id_);
  }

  const std::size_t cipher_size = plaintext_payload.size() + kHmacSha256TagSize;
  const std::size_t predicted_final_size = header_portion.size() + cipher_size;
  {
    const auto size32 = static_cast<std::uint32_t>(predicted_final_size);
    std::memcpy(header_portion.data() + 4, &size32, sizeof(size32));
  }

  std::vector<char> to_sign;
  to_sign.reserve(header_portion.size() + plaintext_payload.size());
  to_sign.insert(to_sign.end(), header_portion.begin(), header_portion.end());
  to_sign.insert(to_sign.end(), plaintext_payload.begin(),
                 plaintext_payload.end());
  const auto signature = crypto::HmacSha256(
      ByteSpan(outbound_keys_.signing_key), ByteSpan(to_sign));

  std::vector<char> encrypted_input;
  encrypted_input.reserve(plaintext_payload.size() + signature.size());
  encrypted_input.insert(encrypted_input.end(), plaintext_payload.begin(),
                         plaintext_payload.end());
  encrypted_input.insert(encrypted_input.end(), signature.begin(),
                         signature.end());
  auto ciphertext =
      crypto::AesCbcEncrypt(ByteSpan(outbound_keys_.encrypting_key),
                            ByteSpan(outbound_keys_.initialization_vector),
                            ByteSpan(encrypted_input));
  if (!ciphertext.ok()) {
    return StatusOr<std::vector<char>>{ciphertext.status()};
  }

  std::vector<char> frame;
  frame.reserve(header_portion.size() + ciphertext->size());
  frame.insert(frame.end(), header_portion.begin(), header_portion.end());
  frame.insert(frame.end(), ciphertext->begin(), ciphertext->end());
  FixUpFrameSize(frame);
  return StatusOr<std::vector<char>>{std::move(frame)};
}

}  // namespace opcua::binary
