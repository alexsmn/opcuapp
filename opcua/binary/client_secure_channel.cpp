#include "opcua/binary/client_secure_channel.h"

#include "opcua/binary/codec_utils.h"

#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace opcua::binary {
namespace {

constexpr std::size_t kRsaOaepSha1Overhead = 42;
constexpr std::size_t kHmacSha256TagSize = 32;
constexpr std::size_t kAesBlockSize = 16;

// scada::ByteString is std::vector<char>, so one overload covers both.
std::span<const std::uint8_t> ByteSpan(const std::vector<char>& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// Rewrite the frame header's message_size field (bytes 4..7, little-endian)
// to match the actual byte count of the assembled frame.
void FixUpFrameSize(std::vector<char>& frame) {
  const auto size = static_cast<std::uint32_t>(frame.size());
  std::memcpy(frame.data() + 4, &size, sizeof(size));
}

}  // namespace

ClientSecureChannel::ClientSecureChannel(ClientTransport& transport)
    : transport_{transport} {}

ClientSecureChannel::ClientSecureChannel(ClientTransport& transport,
                                         Security security)
    : transport_{transport}, security_{std::move(security)} {}

bool ClientSecureChannel::UsesBasic256Sha256() const {
  return security_.security_policy_uri != std::string{kSecurityPolicyNone} &&
         security_.security_mode != MessageSecurityMode::None;
}

bool ClientSecureChannel::UsesSignAndEncrypt() const {
  return security_.security_mode == MessageSecurityMode::SignAndEncrypt;
}

std::uint32_t ClientSecureChannel::NextRequestId() {
  return next_request_id_++;
}

namespace {
// Asymmetric signature algorithm URI for Basic256Sha256 (RSA-PKCS#1-v1.5 with
// SHA-256). OPC UA Part 7.
constexpr std::string_view kRsaSha256SignatureUri =
    "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256";
}  // namespace

scada::StatusOr<ClientSecureChannel::ClientSignature>
ClientSecureChannel::SignClientData(std::span<const std::uint8_t> data) const {
  // Unsecured channel: no client certificate, so no signature (the
  // SignatureData on the wire stays empty).
  if (!UsesBasic256Sha256()) {
    return scada::StatusOr<ClientSignature>{ClientSignature{}};
  }
  if (security_.client_private_key.empty()) {
    return scada::StatusOr<ClientSignature>{
        scada::Status{scada::StatusCode::Bad}};
  }
  auto signature =
      crypto::RsaPkcs1Sha256Sign(security_.client_private_key, data);
  if (!signature.ok()) {
    return scada::StatusOr<ClientSignature>{signature.status()};
  }
  return scada::StatusOr<ClientSignature>{
      ClientSignature{.algorithm = std::string{kRsaSha256SignatureUri},
                      .signature = std::move(*signature)}};
}

std::vector<char> ClientSecureChannel::BuildPlaintextOpenFrame(
    std::uint32_t request_id,
    std::uint32_t request_handle,
    SecurityTokenRequestType request_type,
    std::uint32_t secure_channel_id,
    const scada::ByteString& client_nonce,
    std::uint32_t requested_lifetime_ms) {
  const OpenSecureChannelRequest request{
      .request_header = {.request_handle = request_handle},
      .client_protocol_version = 0,
      .request_type = request_type,
      .security_mode = security_.security_mode,
      .client_nonce = client_nonce,
      .requested_lifetime = requested_lifetime_ms,
  };
  const auto body = EncodeOpenSecureChannelRequestBody(request);

  AsymmetricSecurityHeader asym_header{
      .security_policy_uri = security_.security_policy_uri,
      .sender_certificate = {},
      .receiver_certificate_thumbprint = {},
  };
  if (UsesBasic256Sha256()) {
    auto sender_der = crypto::CertificateDer(security_.client_certificate);
    auto recv_thumb =
        crypto::CertificateThumbprint(security_.server_certificate);
    if (sender_der.ok()) {
      asym_header.sender_certificate = std::move(*sender_der);
    }
    if (recv_thumb.ok()) {
      asym_header.receiver_certificate_thumbprint = std::move(*recv_thumb);
    }
  }

  const SecureConversationMessage frame_message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = secure_channel_id,
      .asymmetric_security_header = std::move(asym_header),
      .symmetric_security_header = std::nullopt,
      .sequence_header = {.sequence_number = next_sequence_number_++,
                          .request_id = request_id},
      .body = body,
  };
  return EncodeSecureConversationMessage(frame_message);
}

scada::StatusOr<std::vector<char>>
ClientSecureChannel::BuildAsymmetricBasic256Sha256OpenFrame(
    std::uint32_t request_id,
    std::uint32_t request_handle,
    SecurityTokenRequestType request_type,
    std::uint32_t secure_channel_id,
    const scada::ByteString& client_nonce,
    std::uint32_t requested_lifetime_ms) {
  // 1. Build the plaintext frame (header + chan_id + asym_header +
  //    seq_header + body). EncodeSecureConversationMessage has already
  //    laid out those fields contiguously.
  auto frame = BuildPlaintextOpenFrame(request_id, request_handle, request_type,
                                       secure_channel_id, client_nonce,
                                       requested_lifetime_ms);

  // 2. Locate the boundary between the plaintext prefix (which stays in
  //    the clear on the wire) and the section that gets encrypted
  //    (seq_header + body + padding + signature). That boundary is the
  //    end of the asymmetric security header. We rebuild it here to
  //    match the exact bytes EncodeSecureConversationMessage wrote.
  std::vector<char> plaintext_prefix;
  {
    Encoder enc{plaintext_prefix};
    // Frame header is the first 8 bytes of `frame` as-is.
    plaintext_prefix.insert(plaintext_prefix.end(), frame.begin(),
                            frame.begin() + 8);
    Encoder tail_enc{plaintext_prefix};
    tail_enc.Encode(secure_channel_id);
    tail_enc.Encode(security_.security_policy_uri);
    auto sender_der = crypto::CertificateDer(security_.client_certificate);
    auto recv_thumb =
        crypto::CertificateThumbprint(security_.server_certificate);
    if (!sender_der.ok() || !recv_thumb.ok()) {
      return scada::StatusOr<std::vector<char>>{
          scada::Status{scada::StatusCode::Bad}};
    }
    tail_enc.Encode(*sender_der);
    tail_enc.Encode(*recv_thumb);
  }
  const std::size_t header_end = plaintext_prefix.size();
  if (frame.size() <= header_end) {
    return scada::StatusOr<std::vector<char>>{
        scada::Status{scada::StatusCode::Bad}};
  }
  // 3. Everything from header_end onward in `frame` is [seq_header][body]
  //    (no padding/signature yet). Compute padding/signature sizes and
  //    assemble the to-encrypt plaintext.
  const std::size_t client_key_bytes =
      static_cast<std::size_t>(security_.client_private_key.KeySizeBytes());
  const std::size_t server_key_bytes = static_cast<std::size_t>(
      crypto::CertificatePublicKey(security_.server_certificate)
          ->KeySizeBytes());
  const std::size_t plain_block = server_key_bytes - kRsaOaepSha1Overhead;
  const std::size_t signature_size = client_key_bytes;

  std::vector<char> to_encrypt(frame.begin() + header_end, frame.end());
  // Pad so (to_encrypt.size + 1 + signature_size) is a multiple of
  // plain_block. One PaddingSize byte comes after the padding bytes.
  const std::size_t unpadded = to_encrypt.size() + 1 + signature_size;
  const std::size_t pad_count =
      (plain_block - unpadded % plain_block) % plain_block;
  if (pad_count > 255) {
    // Would require ExtraPaddingSize; not implemented.
    return scada::StatusOr<std::vector<char>>{
        scada::Status{scada::StatusCode::Bad}};
  }
  to_encrypt.insert(to_encrypt.end(), pad_count, static_cast<char>(pad_count));
  to_encrypt.push_back(static_cast<char>(pad_count));  // PaddingSize byte

  // 4. The signature must cover the frame header *including* the final
  //    message_size field. Compute the expected final size now so that we
  //    can stamp it into the prefix before signing. The OPC UA spec
  //    (Part 6 §6.7.2) requires signature coverage of the whole chunk
  //    header.
  const std::size_t total_plaintext = to_encrypt.size() + signature_size;
  const std::size_t num_blocks = total_plaintext / plain_block;
  const std::size_t cipher_size = num_blocks * server_key_bytes;
  const std::size_t predicted_final_size =
      plaintext_prefix.size() + cipher_size;
  {
    const auto size32 = static_cast<std::uint32_t>(predicted_final_size);
    std::memcpy(plaintext_prefix.data() + 4, &size32, sizeof(size32));
  }

  // 5. Sign [plaintext_prefix + to_encrypt] with our private key.
  std::vector<char> to_sign;
  to_sign.reserve(plaintext_prefix.size() + to_encrypt.size());
  to_sign.insert(to_sign.end(), plaintext_prefix.begin(),
                 plaintext_prefix.end());
  to_sign.insert(to_sign.end(), to_encrypt.begin(), to_encrypt.end());
  auto signature = crypto::RsaPkcs1Sha256Sign(security_.client_private_key,
                                              ByteSpan(to_sign));
  if (!signature.ok()) {
    return scada::StatusOr<std::vector<char>>{signature.status()};
  }
  to_encrypt.insert(to_encrypt.end(), signature->begin(), signature->end());

  // 5. Encrypt `to_encrypt` with server public key (chunked per plain_block).
  auto server_pub = crypto::CertificatePublicKey(security_.server_certificate);
  if (!server_pub.ok()) {
    return scada::StatusOr<std::vector<char>>{server_pub.status()};
  }
  auto ciphertext = crypto::RsaOaepEncrypt(*server_pub, ByteSpan(to_encrypt));
  if (!ciphertext.ok()) {
    return scada::StatusOr<std::vector<char>>{ciphertext.status()};
  }

  // 6. Final frame = plaintext_prefix + ciphertext; fix up message_size.
  std::vector<char> final_frame;
  final_frame.reserve(plaintext_prefix.size() + ciphertext->size());
  final_frame.insert(final_frame.end(), plaintext_prefix.begin(),
                     plaintext_prefix.end());
  final_frame.insert(final_frame.end(), ciphertext->begin(), ciphertext->end());
  FixUpFrameSize(final_frame);
  return scada::StatusOr<std::vector<char>>{std::move(final_frame)};
}

scada::StatusOr<ClientSecureChannel::AsymmetricDecodedResponse>
ClientSecureChannel::DecodeAsymmetricBasic256Sha256OpenFrame(
    const std::vector<char>& frame) {
  // 1. Parse the plaintext prefix: frame header + channel id + asym header.
  if (frame.size() < 8) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  Decoder dec{std::span<const char>{frame}.subspan(8)};
  std::uint32_t channel_id = 0;
  AsymmetricSecurityHeader asym_header;
  if (!dec.Decode(channel_id) || !dec.Decode(asym_header.security_policy_uri) ||
      !dec.Decode(asym_header.sender_certificate) ||
      !dec.Decode(asym_header.receiver_certificate_thumbprint)) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  const std::size_t header_end = 8 + dec.offset();
  if (header_end >= frame.size()) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }

  // 2. Decrypt the ciphertext with our private key.
  std::span<const char> cipher_span{frame.data() + header_end,
                                    frame.size() - header_end};
  auto plaintext = crypto::RsaOaepDecrypt(
      security_.client_private_key,
      {reinterpret_cast<const std::uint8_t*>(cipher_span.data()),
       cipher_span.size()});
  if (!plaintext.ok()) {
    return scada::StatusOr<AsymmetricDecodedResponse>{plaintext.status()};
  }

  // 3. Verify signature. The signed-over region is
  //    [frame[0..header_end)] + [plaintext minus the signature tail].
  //    Signature size = server's cert key size bytes.
  auto server_pub = crypto::CertificatePublicKey(security_.server_certificate);
  if (!server_pub.ok()) {
    return scada::StatusOr<AsymmetricDecodedResponse>{server_pub.status()};
  }
  const std::size_t signature_size =
      static_cast<std::size_t>(server_pub->KeySizeBytes());
  if (plaintext->size() < signature_size + 1) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  const auto sig_begin = plaintext->size() - signature_size;
  std::vector<char> signed_region;
  signed_region.reserve(header_end + sig_begin);
  signed_region.insert(signed_region.end(), frame.begin(),
                       frame.begin() + header_end);
  signed_region.insert(signed_region.end(), plaintext->begin(),
                       plaintext->begin() + sig_begin);
  std::span<const std::uint8_t> signature_bytes{
      reinterpret_cast<const std::uint8_t*>(plaintext->data() + sig_begin),
      signature_size};
  if (!crypto::RsaPkcs1Sha256Verify(*server_pub, ByteSpan(signed_region),
                                    signature_bytes)) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }

  // 4. Strip padding to recover seq_header + body.
  //    plaintext[sig_begin-1] = PaddingSize byte. Preceding pad_count
  //    bytes are all equal to pad_count.
  const auto pad_size = static_cast<std::uint8_t>((*plaintext)[sig_begin - 1]);
  if (sig_begin < static_cast<std::size_t>(1 + pad_size)) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  const auto body_end = sig_begin - 1 - pad_size;

  // 5. Extract seq_header (8 bytes) and body.
  if (body_end < 8) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  AsymmetricDecodedResponse result;
  result.security_header = std::move(asym_header);
  Decoder seq_dec{std::span<const char>{plaintext->data(), 8}};
  if (!seq_dec.Decode(result.sequence_header.sequence_number) ||
      !seq_dec.Decode(result.sequence_header.request_id)) {
    return scada::StatusOr<AsymmetricDecodedResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  result.body.assign(plaintext->begin() + 8, plaintext->begin() + body_end);
  return scada::StatusOr<AsymmetricDecodedResponse>{std::move(result)};
}

scada::StatusOr<scada::ByteString> ClientSecureChannel::GenerateClientNonce() {
  if (security_.client_nonce_generator) {
    auto nonce = security_.client_nonce_generator();
    if (!nonce.ok()) {
      return nonce;
    }
    if (nonce->size() != 32) {
      return scada::StatusOr<scada::ByteString>{
          scada::Status{scada::StatusCode::Bad}};
    }
    return nonce;
  }

  scada::ByteString nonce(32, 0);
  if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()),
                 static_cast<int>(nonce.size())) != 1) {
    return scada::StatusOr<scada::ByteString>{
        scada::Status{scada::StatusCode::Bad}};
  }
  return scada::StatusOr<scada::ByteString>{std::move(nonce)};
}

bool ClientSecureChannel::ShouldRenew() const {
  return opened_ && std::chrono::steady_clock::now() >= renew_at_;
}

void ClientSecureChannel::ArmRenewalTimer(std::uint32_t revised_lifetime_ms) {
  revised_lifetime_ms_ = revised_lifetime_ms;
  if (revised_lifetime_ms_ == 0) {
    renew_at_ = std::chrono::steady_clock::time_point{};
    return;
  }

  const auto renew_after_ms = std::max<std::uint64_t>(
      1, (static_cast<std::uint64_t>(revised_lifetime_ms_) * 3) / 4);
  const auto renew_after = std::chrono::milliseconds{
      static_cast<std::chrono::milliseconds::rep>(renew_after_ms)};
  renew_at_ = std::chrono::steady_clock::now() + renew_after;
}

Awaitable<scada::Status> ClientSecureChannel::Open(
    std::uint32_t requested_lifetime_ms) {
  co_return co_await OpenSecureChannel(SecurityTokenRequestType::Issue,
                                       requested_lifetime_ms);
}

Awaitable<scada::Status> ClientSecureChannel::Renew(
    std::uint32_t requested_lifetime_ms) {
  if (!opened_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  co_return co_await OpenSecureChannel(SecurityTokenRequestType::Renew,
                                       requested_lifetime_ms);
}

Awaitable<scada::Status> ClientSecureChannel::RenewIfNeeded() {
  if (!ShouldRenew()) {
    co_return scada::Status{scada::StatusCode::Good};
  }
  co_return co_await Renew(revised_lifetime_ms_);
}

Awaitable<scada::Status> ClientSecureChannel::OpenSecureChannel(
    SecurityTokenRequestType request_type,
    std::uint32_t requested_lifetime_ms) {
  const std::uint32_t request_id = NextRequestId();
  const std::uint32_t request_handle = request_id;
  const std::uint32_t secure_channel_id =
      request_type == SecurityTokenRequestType::Renew ? channel_id_ : 0;

  // For Basic256Sha256 we generate a 32-byte client nonce that's fed into
  // the symmetric KDF once the server nonce comes back.
  scada::ByteString client_nonce;
  if (UsesBasic256Sha256()) {
    auto generated = GenerateClientNonce();
    if (!generated.ok()) {
      co_return generated.status();
    }
    client_nonce = std::move(*generated);
    client_nonce_ = client_nonce;
  }

  std::vector<char> out_frame;
  if (UsesBasic256Sha256()) {
    auto framed = BuildAsymmetricBasic256Sha256OpenFrame(
        request_id, request_handle, request_type, secure_channel_id,
        client_nonce, requested_lifetime_ms);
    if (!framed.ok()) {
      co_return framed.status();
    }
    out_frame = std::move(*framed);
  } else {
    out_frame = BuildPlaintextOpenFrame(request_id, request_handle,
                                        request_type, secure_channel_id,
                                        client_nonce, requested_lifetime_ms);
  }
  auto write_status = co_await transport_.WriteFrame(out_frame);
  if (write_status.bad()) {
    co_return write_status;
  }

  auto read_frame = co_await transport_.ReadFrame();
  if (!read_frame.ok()) {
    co_return read_frame.status();
  }

  OpenSecureChannelResponse response;
  if (UsesBasic256Sha256()) {
    auto decoded = DecodeAsymmetricBasic256Sha256OpenFrame(*read_frame);
    if (!decoded.ok()) {
      co_return decoded.status();
    }
    auto body = DecodeOpenSecureChannelResponseBody(decoded->body);
    if (!body.has_value()) {
      co_return scada::Status{scada::StatusCode::Bad};
    }
    response = std::move(*body);
  } else {
    const auto message = DecodeSecureConversationMessage(*read_frame);
    if (!message.has_value() ||
        message->frame_header.message_type != MessageType::SecureOpen ||
        !message->asymmetric_security_header.has_value() ||
        message->asymmetric_security_header->security_policy_uri !=
            kSecurityPolicyNone) {
      co_return scada::Status{scada::StatusCode::Bad};
    }
    auto body = DecodeOpenSecureChannelResponseBody(message->body);
    if (!body.has_value()) {
      co_return scada::Status{scada::StatusCode::Bad};
    }
    response = std::move(*body);
  }

  if (response.response_header.service_result.bad()) {
    co_return response.response_header.service_result;
  }

  channel_id_ = response.security_token.channel_id;
  token_id_ = response.security_token.token_id;
  ArmRenewalTimer(response.security_token.revised_lifetime);

  if (UsesBasic256Sha256()) {
    // OPC UA Part 6 §6.7.5 key derivation:
    //   ClientKeys = P_SHA256(ServerNonce, ClientNonce)
    //   ServerKeys = P_SHA256(ClientNonce, ServerNonce)
    client_keys_ = crypto::DeriveBasic256Sha256Keys(
        ByteSpan(response.server_nonce), ByteSpan(client_nonce_));
    server_keys_ = crypto::DeriveBasic256Sha256Keys(
        ByteSpan(client_nonce_), ByteSpan(response.server_nonce));
  }

  opened_ = true;
  co_return scada::Status{scada::StatusCode::Good};
}

scada::StatusOr<std::vector<char>>
ClientSecureChannel::BuildSymmetricBasic256Sha256Frame(
    MessageType type,
    std::uint32_t request_id,
    const std::vector<char>& body) {
  // 1. Assemble the plaintext prefix (frame header, channel_id, sym header)
  //    plus the plaintext payload [seq_header][body]. We'll re-encode once
  //    padding+signature are known.
  //
  // Plaintext for encrypt: [seq_header 8B][body][padding][PaddingSize 1B]
  // where padding ensures (payload + 1 + 32 sig) % 16 == 0.
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
    return scada::StatusOr<std::vector<char>>{
        scada::Status{scada::StatusCode::Bad}};
  }
  plaintext_payload.insert(plaintext_payload.end(), pad_count,
                           static_cast<char>(pad_count));
  plaintext_payload.push_back(static_cast<char>(pad_count));

  // 2. Build the header portion (8-byte frame header + channel_id + sym
  //    header) so we can compute the signature over the whole plaintext
  //    frame.
  std::vector<char> header_portion;
  {
    // Frame header: 4-byte type tag + 4-byte message_size (fixed later).
    const char* type_tag = nullptr;
    switch (type) {
      case MessageType::SecureMessage:
        type_tag = "MSGF";
        break;
      case MessageType::SecureClose:
        type_tag = "CLOF";
        break;
      default:
        return scada::StatusOr<std::vector<char>>{
            scada::Status{scada::StatusCode::Bad}};
    }
    header_portion.insert(header_portion.end(), type_tag, type_tag + 4);
    const std::uint32_t placeholder_size = 0;
    header_portion.insert(header_portion.end(),
                          reinterpret_cast<const char*>(&placeholder_size),
                          reinterpret_cast<const char*>(&placeholder_size) + 4);
    Encoder enc{header_portion};
    enc.Encode(channel_id_);
    enc.Encode(token_id_);
  }

  // 3. Pre-compute the final frame size so the HMAC covers the final
  //    message_size field (AES-256-CBC with no padding preserves input
  //    length, so cipher_size = plaintext_payload.size + sig_size).
  const std::size_t cipher_size = plaintext_payload.size() + kHmacSha256TagSize;
  const std::size_t predicted_final_size = header_portion.size() + cipher_size;
  {
    const auto size32 = static_cast<std::uint32_t>(predicted_final_size);
    std::memcpy(header_portion.data() + 4, &size32, sizeof(size32));
  }

  // 4. Sign [header_portion + plaintext_payload] with the client's HMAC key.
  std::vector<char> to_sign;
  to_sign.reserve(header_portion.size() + plaintext_payload.size());
  to_sign.insert(to_sign.end(), header_portion.begin(), header_portion.end());
  to_sign.insert(to_sign.end(), plaintext_payload.begin(),
                 plaintext_payload.end());
  const auto signature =
      crypto::HmacSha256(ByteSpan(client_keys_.signing_key), ByteSpan(to_sign));

  // 4. Append signature to the plaintext payload, then encrypt.
  std::vector<char> encrypted_input;
  encrypted_input.reserve(plaintext_payload.size() + signature.size());
  encrypted_input.insert(encrypted_input.end(), plaintext_payload.begin(),
                         plaintext_payload.end());
  encrypted_input.insert(encrypted_input.end(), signature.begin(),
                         signature.end());
  auto ciphertext = crypto::AesCbcEncrypt(
      ByteSpan(client_keys_.encrypting_key),
      ByteSpan(client_keys_.initialization_vector), ByteSpan(encrypted_input));
  if (!ciphertext.ok()) {
    return scada::StatusOr<std::vector<char>>{ciphertext.status()};
  }

  // 5. Final frame = header_portion + ciphertext; fix up message_size.
  std::vector<char> frame;
  frame.reserve(header_portion.size() + ciphertext->size());
  frame.insert(frame.end(), header_portion.begin(), header_portion.end());
  frame.insert(frame.end(), ciphertext->begin(), ciphertext->end());
  FixUpFrameSize(frame);
  return scada::StatusOr<std::vector<char>>{std::move(frame)};
}

scada::StatusOr<ClientSecureChannel::ServiceResponse>
ClientSecureChannel::DecodeSymmetricBasic256Sha256Frame(
    const std::vector<char>& frame) {
  // Header: 4-byte type + 4-byte size + 4-byte channel_id + 4-byte token_id
  // = 16 bytes.
  constexpr std::size_t kHeaderSize = 16;
  if (frame.size() < kHeaderSize) {
    return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  // Validate channel_id / token_id.
  std::uint32_t channel_id = 0;
  std::uint32_t token_id = 0;
  std::memcpy(&channel_id, frame.data() + 8, 4);
  std::memcpy(&token_id, frame.data() + 12, 4);
  if (channel_id != channel_id_ || token_id != token_id_) {
    return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }

  // Decrypt the post-header bytes with the server's encrypting key.
  std::span<const char> cipher_span{frame.data() + kHeaderSize,
                                    frame.size() - kHeaderSize};
  auto decrypted = crypto::AesCbcDecrypt(
      ByteSpan(server_keys_.encrypting_key),
      ByteSpan(server_keys_.initialization_vector),
      {reinterpret_cast<const std::uint8_t*>(cipher_span.data()),
       cipher_span.size()});
  if (!decrypted.ok()) {
    return scada::StatusOr<ServiceResponse>{decrypted.status()};
  }
  if (decrypted->size() < kHmacSha256TagSize) {
    return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }

  // Last 32 bytes = HMAC signature; verify over [header +
  // plaintext_before_sig].
  const auto sig_begin = decrypted->size() - kHmacSha256TagSize;
  std::vector<char> signed_region;
  signed_region.reserve(kHeaderSize + sig_begin);
  signed_region.insert(signed_region.end(), frame.begin(),
                       frame.begin() + kHeaderSize);
  signed_region.insert(signed_region.end(), decrypted->begin(),
                       decrypted->begin() + sig_begin);
  const auto expected_tag = crypto::HmacSha256(
      ByteSpan(server_keys_.signing_key), ByteSpan(signed_region));
  if (expected_tag.size() != kHmacSha256TagSize ||
      std::memcmp(expected_tag.data(), decrypted->data() + sig_begin,
                  kHmacSha256TagSize) != 0) {
    return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }

  // Strip padding: last byte before sig is PaddingSize.
  const auto pad_size = static_cast<std::uint8_t>((*decrypted)[sig_begin - 1]);
  if (sig_begin < static_cast<std::size_t>(1 + pad_size) + 8) {
    return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  const auto body_end = sig_begin - 1 - pad_size;

  // First 8 bytes of decrypted = seq_header (sequence_number + request_id).
  std::uint32_t sequence_number = 0;
  std::uint32_t request_id = 0;
  std::memcpy(&sequence_number, decrypted->data(), 4);
  std::memcpy(&request_id, decrypted->data() + 4, 4);

  ServiceResponse response;
  response.request_id = request_id;
  response.body.assign(decrypted->begin() + 8, decrypted->begin() + body_end);
  return scada::StatusOr<ServiceResponse>{std::move(response)};
}

Awaitable<scada::Status> ClientSecureChannel::SendServiceRequest(
    std::uint32_t request_id,
    const std::vector<char>& body) {
  if (!opened_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  const auto renew_status = co_await RenewIfNeeded();
  if (renew_status.bad()) {
    co_return renew_status;
  }
  if (UsesSignAndEncrypt()) {
    auto framed = BuildSymmetricBasic256Sha256Frame(MessageType::SecureMessage,
                                                    request_id, body);
    if (!framed.ok()) {
      co_return framed.status();
    }
    co_return co_await transport_.WriteFrame(*framed);
  }
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = channel_id_,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          SymmetricSecurityHeader{.token_id = token_id_},
      .sequence_header = {.sequence_number = next_sequence_number_++,
                          .request_id = request_id},
      .body = body,
  };
  co_return co_await transport_.WriteFrame(
      EncodeSecureConversationMessage(message));
}

scada::StatusOr<ClientSecureChannel::ServiceResponse>
ClientSecureChannel::DecodeServiceMessageChunk(const std::vector<char>& frame) {
  if (UsesSignAndEncrypt()) {
    return DecodeSymmetricBasic256Sha256Frame(frame);
  }
  const auto message = DecodeSecureConversationMessage(frame);
  if (!message.has_value() ||
      message->frame_header.message_type != MessageType::SecureMessage ||
      message->secure_channel_id != channel_id_ ||
      !message->symmetric_security_header.has_value() ||
      message->symmetric_security_header->token_id != token_id_) {
    return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad}};
  }
  return scada::StatusOr<ServiceResponse>{ServiceResponse{
      .request_id = message->sequence_header.request_id,
      .body = std::vector<char>{message->body.begin(), message->body.end()}}};
}

Awaitable<scada::StatusOr<ClientSecureChannel::ServiceResponse>>
ClientSecureChannel::ReadServiceResponse() {
  if (!opened_) {
    co_return scada::StatusOr<ServiceResponse>{
        scada::Status{scada::StatusCode::Bad_Disconnected}};
  }

  // A single OPC UA service response may be split across several MessageChunks
  // ('C' ... 'C' 'F'); their decoded bodies are concatenated before the result
  // is handed up for decoding (OPC UA Part 6 §6.7.2/§6.7.3). 'A' aborts the
  // message. The caps bound memory against a malformed or hostile server.
  constexpr char kIntermediateChunk = 'C';
  constexpr char kFinalChunk = 'F';
  constexpr std::size_t kMaxResponseChunks = 8192;
  constexpr std::size_t kMaxResponseBytes = 64u * 1024 * 1024;

  std::vector<char> reassembled;
  std::uint32_t request_id = 0;
  for (std::size_t chunk_index = 0;; ++chunk_index) {
    if (chunk_index >= kMaxResponseChunks) {
      co_return scada::StatusOr<ServiceResponse>{
          scada::Status{scada::StatusCode::Bad}};
    }
    auto frame = co_await transport_.ReadFrame();
    if (!frame.ok()) {
      co_return scada::StatusOr<ServiceResponse>{frame.status()};
    }
    // The chunk type lives in the unencrypted frame header (byte 3) for every
    // message type, so it can be read before decoding the chunk.
    if (frame->size() < 4) {
      co_return scada::StatusOr<ServiceResponse>{
          scada::Status{scada::StatusCode::Bad}};
    }
    const char chunk_type = (*frame)[3];
    if (chunk_type != kIntermediateChunk && chunk_type != kFinalChunk) {
      // 'A' (abort) or an unknown chunk type: discard the whole message.
      co_return scada::StatusOr<ServiceResponse>{
          scada::Status{scada::StatusCode::Bad}};
    }

    auto chunk = DecodeServiceMessageChunk(*frame);
    if (!chunk.ok()) {
      co_return scada::StatusOr<ServiceResponse>{chunk.status()};
    }
    request_id = chunk->request_id;
    if (reassembled.size() + chunk->body.size() > kMaxResponseBytes) {
      co_return scada::StatusOr<ServiceResponse>{
          scada::Status{scada::StatusCode::Bad}};
    }
    reassembled.insert(reassembled.end(), chunk->body.begin(),
                       chunk->body.end());
    if (chunk_type == kFinalChunk) {
      break;
    }
  }
  co_return scada::StatusOr<ServiceResponse>{ServiceResponse{
      .request_id = request_id, .body = std::move(reassembled)}};
}

Awaitable<scada::Status> ClientSecureChannel::Close() {
  if (!opened_) {
    co_return scada::Status{scada::StatusCode::Good};
  }
  const std::uint32_t request_id = NextRequestId();
  const CloseSecureChannelRequest close_request{
      .request_header = {.request_handle = request_id}};
  const auto body = EncodeCloseSecureChannelRequestBody(close_request);

  scada::Status status{scada::StatusCode::Good};
  if (UsesSignAndEncrypt()) {
    auto framed = BuildSymmetricBasic256Sha256Frame(MessageType::SecureClose,
                                                    request_id, body);
    if (framed.ok()) {
      status = co_await transport_.WriteFrame(*framed);
    } else {
      status = framed.status();
    }
  } else {
    const SecureConversationMessage message{
        .frame_header = {.message_type = MessageType::SecureClose,
                         .chunk_type = 'F',
                         .message_size = 0},
        .secure_channel_id = channel_id_,
        .asymmetric_security_header = std::nullopt,
        .symmetric_security_header =
            SymmetricSecurityHeader{.token_id = token_id_},
        .sequence_header = {.sequence_number = next_sequence_number_++,
                            .request_id = request_id},
        .body = body,
    };
    status = co_await transport_.WriteFrame(
        EncodeSecureConversationMessage(message));
  }
  opened_ = false;
  co_return status;
}

}  // namespace opcua::binary
