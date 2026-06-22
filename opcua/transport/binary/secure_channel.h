#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/scada/basic_types.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opcua::binary {

constexpr std::uint32_t kOpenSecureChannelRequestEncodingId = 446;
constexpr std::uint32_t kOpenSecureChannelResponseEncodingId = 449;
constexpr std::uint32_t kCloseSecureChannelRequestEncodingId = 452;
constexpr std::uint32_t kCloseSecureChannelResponseEncodingId = 455;

constexpr std::string_view kSecurityPolicyNone =
    "http://opcfoundation.org/UA/SecurityPolicy#None";

// SecurityPolicy URI for Basic256Sha256 (OPC UA Part 7). Asymmetric:
// RSA-OAEP-SHA1 + RSA-PKCS#1-v1.5-SHA256; symmetric: AES-256-CBC +
// HMAC-SHA256 with keys derived from the OPN nonce exchange.
constexpr std::string_view kSecurityPolicyBasic256Sha256 =
    "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256";

enum class SecurityTokenRequestType : std::uint32_t {
  Issue = 0,
  Renew = 1,
};

enum class MessageSecurityMode : std::uint32_t {
  Invalid = 0,
  None = 1,
  Sign = 2,
  SignAndEncrypt = 3,
};

struct RequestHeader {
  std::uint32_t request_handle = 0;
  std::uint32_t return_diagnostics = 0;
  std::string audit_entry_id;
  std::uint32_t timeout_hint = 0;
};

struct ResponseHeader {
  std::uint32_t request_handle = 0;
  Status service_result = StatusCode::Good;
};

struct ChannelSecurityToken {
  std::uint32_t channel_id = 0;
  std::uint32_t token_id = 0;
  std::int64_t created_at = 0;
  std::uint32_t revised_lifetime = 0;
};

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

struct OpenSecureChannelRequest {
  RequestHeader request_header;
  std::uint32_t client_protocol_version = 0;
  SecurityTokenRequestType request_type =
      SecurityTokenRequestType::Issue;
  MessageSecurityMode security_mode =
      MessageSecurityMode::None;
  ByteString client_nonce;
  std::uint32_t requested_lifetime = 0;
};

struct OpenSecureChannelResponse {
  ResponseHeader response_header;
  std::uint32_t server_protocol_version = 0;
  ChannelSecurityToken security_token;
  ByteString server_nonce;
};

struct CloseSecureChannelRequest {
  RequestHeader request_header;
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
  // Whether the insecure SecurityPolicy=None endpoint is accepted.
  bool allow_none = true;
  // Whether Basic256Sha256 SignAndEncrypt is accepted (requires certificate).
  bool allow_basic256sha256 = false;
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
  };

  explicit SecureChannel(std::uint32_t channel_id = 1);
  // Secured channel. A null config behaves like the SecurityPolicy=None-only
  // channel above.
  explicit SecureChannel(
      std::shared_ptr<const SecureChannelServerConfig> config,
      std::uint32_t channel_id = 1);

  [[nodiscard]] Awaitable<Result> HandleFrame(std::vector<char> frame);
  [[nodiscard]] std::vector<char> BuildServiceResponse(
      std::uint32_t request_id,
      std::vector<char> body);

  [[nodiscard]] bool opened() const { return opened_; }
  [[nodiscard]] std::uint32_t channel_id() const { return channel_id_; }
  [[nodiscard]] std::uint32_t token_id() const { return token_id_; }
  // True once the channel negotiated Basic256Sha256 SignAndEncrypt.
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
  [[nodiscard]] Result HandleOpenSecure(const std::vector<char>& frame);
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
  std::uint32_t next_sequence_number_ = 1;
  bool opened_ = false;

  // Basic256Sha256 SignAndEncrypt state. inbound_keys_ verify/decrypt client
  // traffic; outbound_keys_ sign/encrypt server traffic (OPC UA Part 6 §6.7.5).
  bool basic256_active_ = false;
  crypto::DerivedKeys inbound_keys_;
  crypto::DerivedKeys outbound_keys_;
  ByteString server_nonce_;
  ByteString client_certificate_der_;
};

}  // namespace opcua::binary
