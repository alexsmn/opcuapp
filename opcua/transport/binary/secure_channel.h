#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/types/basic_types.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"
#include "opcua/ua/ua_types.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opcua::binary {

constexpr std::string_view kSecurityPolicyNone =
    "http://opcfoundation.org/UA/SecurityPolicy#None";

// SecurityPolicy URI for Basic256Sha256 (OPC UA Part 7). Asymmetric:
// RSA-OAEP-SHA1 + RSA-PKCS#1-v1.5-SHA256; symmetric: AES-256-CBC +
// HMAC-SHA256 with keys derived from the OPN nonce exchange.
constexpr std::string_view kSecurityPolicyBasic256Sha256 =
    "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256";

// SecurityPolicy URI for Aes128_Sha256_RsaOaep, the [A] RSA policy. The same
// suite as Basic256Sha256 -- RSA-OAEP-SHA1, RSA-PKCS#1-v1.5-SHA256, P_SHA256,
// HMAC-SHA256, a 256-bit derived signing key -- except that the symmetric
// cipher is AES-128-CBC, so the derived encrypting key is 16 bytes. Read from
// the profile "SecurityPolicy [A] - Aes128-Sha256-RsaOaep",
// https://profiles.opcfoundation.org/profile/2058 (Released; verified
// 2026-10-24).
constexpr std::string_view kSecurityPolicyAes128Sha256RsaOaep =
    "http://opcfoundation.org/UA/SecurityPolicy#Aes128_Sha256_RsaOaep";

// The derived symmetric encrypting key length, in bytes, of an RSA
// SecurityPolicy this stack implements; zero for any other URI.
[[nodiscard]] constexpr std::size_t EncryptingKeyLengthFor(
    std::string_view policy_uri) {
  if (policy_uri == kSecurityPolicyBasic256Sha256)
    return 32;
  if (policy_uri == kSecurityPolicyAes128Sha256RsaOaep)
    return 16;
  return 0;
}

// The SecureChannel handshake *message bodies* are the generated types — see
// opcua/CLAUDE.md, "The type system is generated, not transcribed". They used
// to be hand-transcribed structs here, alongside hand-written encoding-id
// constants; the ids now come from each message's own `kBinaryEncodingId`, so
// there is no second place for the wire layout to drift from the schema.
using ua::CloseSecureChannelRequest;
using ua::MessageSecurityMode;
using ua::OpenSecureChannelRequest;
using ua::OpenSecureChannelResponse;
using ua::SecurityTokenRequestType;

// The *framing* below stays hand-written, and deliberately so: the
// SecureConversationMessage and its security/sequence headers are transport
// framing from OPC UA Part 6 §6.7, not StructuredTypes in the schema, so the
// generator has no counterpart to emit for them.
struct AsymmetricSecurityHeader {
  std::string security_policy_uri;
  ByteString sender_certificate;
  ByteString receiver_certificate_thumbprint;
};

struct SymmetricSecurityHeader {
  std::uint32_t token_id = 0;
};

struct SequenceHeader {
  std::uint32_t sequence_number = 0;
  std::uint32_t request_id = 0;
};

struct SecureConversationMessage {
  FrameHeader frame_header;
  std::uint32_t secure_channel_id = 0;
  std::optional<AsymmetricSecurityHeader> asymmetric_security_header;
  std::optional<SymmetricSecurityHeader> symmetric_security_header;
  SequenceHeader sequence_header;
  std::vector<char> body;
};

[[nodiscard]] std::optional<SecureConversationMessage>
DecodeSecureConversationMessage(const std::vector<char>& frame);
[[nodiscard]] std::vector<char> EncodeSecureConversationMessage(
    const SecureConversationMessage& message);

[[nodiscard]] std::optional<OpenSecureChannelRequest>
DecodeOpenSecureChannelRequestBody(const std::vector<char>& body);
[[nodiscard]] std::vector<char> EncodeOpenSecureChannelResponseBody(
    const OpenSecureChannelResponse& response);
[[nodiscard]] std::optional<CloseSecureChannelRequest>
DecodeCloseSecureChannelRequestBody(const std::vector<char>& body);

// Client-side inverses: these encode/decode the complementary direction of
// the OPC UA Part 6 SecureChannel request/response pairs, so the new UA
// Binary client under common/opcua/binary/ can talk to the existing
// server-side SecureChannel using the same bytes on the wire.
[[nodiscard]] std::vector<char> EncodeOpenSecureChannelRequestBody(
    const OpenSecureChannelRequest& request);
[[nodiscard]] std::optional<OpenSecureChannelResponse>
DecodeOpenSecureChannelResponseBody(const std::vector<char>& body);
[[nodiscard]] std::vector<char> EncodeCloseSecureChannelRequestBody(
    const CloseSecureChannelRequest& request);

// Per-server SecureChannel configuration shared by every connection. Holds
// the server application instance certificate + private key and the trust /
// policy decisions used when accepting an OpenSecureChannel. It is move-only
// (crypto::Certificate / crypto::PrivateKey own OpenSSL handles), so it is
// shared across connections behind a std::shared_ptr.
struct SecureChannelServerConfig {
  // Server application instance certificate and matching private key. Both are
  // required to offer Basic256Sha256; when empty only SecurityPolicy=None is
  // accepted.
  crypto::Certificate certificate;
  crypto::PrivateKey private_key;
  // DER encoding and SHA-1 thumbprint of `certificate`, cached by
  // MakeSecureChannelServerConfig so the secure-channel hot path avoids
  // recomputing them per OpenSecureChannel.
  ByteString certificate_der;
  ByteString certificate_thumbprint;
  // Whether the insecure SecurityPolicy=None endpoint is offered for sessions.
  //
  // A None SecureChannel is accepted either way, because discovery runs over
  // one: "Every Server shall have a DiscoveryEndpoint that Clients can access
  // without establishing a Session" and "DiscoveryEndpoints shall not require
  // any message security" (OPC UA Part 4 §5.5.1 Overview,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.5.1). What
  // `false` withholds is everything else on that channel, which the runtime
  // refuses (ServerRuntimeContext::require_secure_channel_for_sessions). It
  // used to refuse the None OpenSecureChannel itself, which also refused every
  // client's GetEndpoints -- so no client, ours included, could discover a
  // server configured without None.
  bool allow_none = true;
  // Whether Basic256Sha256 / Aes128_Sha256_RsaOaep SignAndEncrypt are
  // accepted (each requires the certificate).
  bool allow_basic256sha256 = false;
  bool allow_aes128_sha256_rsaoaep = false;
  // Validates a client application instance certificate (DER) presented in the
  // asymmetric OpenSecureChannel header. Returns a bad Status to reject the
  // channel; the default accepts any certificate. A real deployment plugs the
  // trusted/issuer/rejected/CRL store check here.
  std::function<Status(std::span<const std::uint8_t>)>
      validate_client_certificate;
  // 32-byte server nonce generator; defaults to the platform CSPRNG.
  std::function<StatusOr<ByteString>()> server_nonce_generator;
};

// Builds a shared SecureChannelServerConfig from a loaded certificate and
// private key, computing the cached DER / thumbprint. Returns a bad Status if
// the DER or thumbprint cannot be derived from the certificate.
[[nodiscard]] StatusOr<std::shared_ptr<const SecureChannelServerConfig>>
MakeSecureChannelServerConfig(
    crypto::Certificate certificate,
    crypto::PrivateKey private_key,
    bool allow_none = true,
    std::function<Status(std::span<const std::uint8_t>)>
        validate_client_certificate = {});

class SecureChannel {
 public:
  struct Result {
    std::optional<std::vector<char>> outbound_frame;
    std::optional<std::vector<char>> service_payload;
    std::optional<std::uint32_t> request_id;
    bool close_transport = false;
    // True when close_transport is the normal reaction to a well-formed
    // CloseSecureChannel (OPC UA Part 4 §5.5.3: no response is sent, the
    // server just closes), so the transport can distinguish it from a
    // protocol-error close and skip diagnostics.
    bool graceful_close = false;
    // Why close_transport is set: which of the checks in HandleFrame the frame
    // failed, as a short fixed phrase. Always a string literal, so the view
    // never dangles. It exists because the transport logs every such close
    // with one message, and until 2026-09-08 that message carried no reason --
    // 36 distinct rejections (bad token id, thumbprint mismatch, signature
    // failure, decrypt failure, malformed header, ...) were indistinguishable
    // in the log, so the frame that made a demo tier hang up on the
    // aggregating proxy could not be attributed even with a full capture in
    // hand (backlog 647).
    std::string_view close_reason;
  };

  explicit SecureChannel(std::uint32_t channel_id = 1);
  // Secured channel. A null config behaves like the SecurityPolicy=None-only
  // channel above.
  explicit SecureChannel(
      std::shared_ptr<const SecureChannelServerConfig> config,
      std::uint32_t channel_id = 1);

  [[nodiscard]] Awaitable<Result> HandleFrame(std::vector<char> frame);
  [[nodiscard]] std::vector<char> BuildServiceResponse(std::uint32_t request_id,
                                                       std::vector<char> body);

  [[nodiscard]] bool opened() const { return opened_; }
  [[nodiscard]] std::uint32_t channel_id() const { return channel_id_; }
  [[nodiscard]] std::uint32_t token_id() const { return token_id_; }
  // True once the channel negotiated an RSA SecurityPolicy, SignAndEncrypt.
  [[nodiscard]] bool secure() const { return basic256_active_; }
  // The client application instance certificate (DER) presented during a
  // secured OpenSecureChannel. Empty under SecurityPolicy=None. Used by the
  // session layer to verify the ActivateSession clientSignature.
  [[nodiscard]] const ByteString& client_certificate() const {
    return client_certificate_der_;
  }

 private:
  // SecurityPolicy=None OpenSecureChannel handling (no crypto transforms).
  [[nodiscard]] Result HandleOpenNone(const std::vector<char>& frame);
  // Basic256Sha256 SignAndEncrypt OpenSecureChannel handling.
  [[nodiscard]] Result HandleOpenSecure(const std::vector<char>& frame,
                                        std::string_view policy_uri);
  // Symmetric (MSG / CLO) handling under Basic256Sha256 SignAndEncrypt.
  [[nodiscard]] Result HandleSecureMessage(const std::vector<char>& frame,
                                           bool is_close);

  [[nodiscard]] std::vector<char> BuildOpenResponse(
      const SecureConversationMessage& request_message,
      const OpenSecureChannelRequest& request,
      Status service_result);
  // Encrypted + signed OPN response under Basic256Sha256, encrypting to
  // `client_public_key` and signing with the server private key.
  [[nodiscard]] StatusOr<std::vector<char>> BuildSecureOpenResponse(
      const OpenSecureChannelRequest& request,
      std::uint32_t request_id,
      const crypto::PrivateKey& client_public_key,
      const ByteString& client_certificate_thumbprint,
      const ByteString& server_nonce);
  // Symmetric SignAndEncrypt frame for an outbound MSG body.
  [[nodiscard]] StatusOr<std::vector<char>> BuildSecureServiceResponse(
      std::uint32_t request_id,
      const std::vector<char>& body);

  std::shared_ptr<const SecureChannelServerConfig> config_;
  std::uint32_t channel_id_;
  std::uint32_t token_id_ = 1;
  // The token superseded by the last Renew. The server shall keep accepting
  // messages secured with the previous token while the client transitions to
  // the renewed one (OPC UA Part 6 §6.7.4 / Part 4 §5.5.2,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.5.2); 0 = none.
  std::uint32_t previous_token_id_ = 0;
  std::uint32_t next_sequence_number_ = 1;
  bool opened_ = false;

  // RSA SignAndEncrypt state. inbound_keys_ verify/decrypt client traffic;
  // outbound_keys_ sign/encrypt server traffic (OPC UA Part 6 §6.7.6 Deriving
  // keys, https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7.6).
  bool basic256_active_ = false;
  // The SecurityPolicy the channel was opened with; a Renew must keep it.
  std::string active_policy_uri_;
  crypto::DerivedKeys inbound_keys_;
  crypto::DerivedKeys outbound_keys_;
  ByteString server_nonce_;
  ByteString client_certificate_der_;
};

}  // namespace opcua::binary
