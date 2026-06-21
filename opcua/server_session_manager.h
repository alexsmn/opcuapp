#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/base/time/time.h"
#include "opcua/scada/authentication.h"
#include "opcua/scada/service_context.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>

namespace opcua {

struct CreateSessionRequest {
  base::TimeDelta requested_timeout = base::TimeDelta::FromMinutes(10);
  // Client application instance certificate (DER) and a fresh client nonce.
  // Empty under SecurityPolicy=None; populated for a secured session so the
  // server can verify the ActivateSession clientSignature (OPC UA Part 4
  // §5.6.2).
  ByteString client_certificate;
  ByteString client_nonce;
  // SecureChannel binding, filled by the runtime from the connection (not on
  // the wire). When `channel_secure` is set the server requires the body's
  // `client_certificate` to match `channel_certificate` (the certificate the
  // SecureChannel already validated), guarding against a client presenting a
  // different certificate at the session layer than at the channel layer.
  bool channel_secure = false;
  ByteString channel_certificate;
};

struct CreateSessionResponse {
  Status status{StatusCode::Good};
  NodeId session_id;
  NodeId authentication_token;
  ByteString server_nonce;
  // Server application instance certificate (DER). The client signs
  // (server_certificate || server_nonce) in ActivateSession.
  ByteString server_certificate;
  base::TimeDelta revised_timeout;
};

struct ActivateSessionRequest {
  NodeId session_id;
  NodeId authentication_token;
  std::optional<LocalizedText> user_name;
  std::optional<LocalizedText> password;
  bool delete_existing = false;
  bool allow_anonymous = false;
  // clientSignature (SignatureData): the client's signature over
  // (server_certificate || server_nonce) using the SecureChannel's asymmetric
  // signature algorithm. Empty under SecurityPolicy=None.
  std::string client_signature_algorithm;
  ByteString client_signature;
  // Encrypted UserNameIdentityToken password. When
  // `password_encryption_algorithm` is non-empty the password is not in
  // `password` above but here as RSA-OAEP ciphertext of
  // [length(UInt32 LE) || password || server_nonce] under the server
  // certificate (OPC UA Part 4 §7.36). The manager decrypts it.
  ByteString encrypted_password;
  std::string password_encryption_algorithm;
};

struct ActivateSessionResponse {
  Status status{StatusCode::Good};
  ServiceContext service_context;
  std::optional<AuthenticationResult> authentication_result;
  bool resumed = false;
};

struct CloseSessionRequest {
  NodeId session_id;
  NodeId authentication_token;
};

struct CloseSessionResponse {
  Status status{StatusCode::Good};
};

struct ServerSessionLookupResult {
  NodeId session_id;
  NodeId authentication_token;
  ServiceContext service_context;
  std::optional<AuthenticationResult> authentication_result;
  bool attached = false;
  bool activated = false;
};

struct ServerSessionManagerContext {
  std::shared_ptr<CoroutineAuthenticator> authenticator;
  // Server application instance certificate (DER). When non-empty, the client
  // signs (server_certificate || server_nonce) and the manager verifies that
  // clientSignature in ActivateSession. Empty under SecurityPolicy=None.
  ByteString server_certificate;
  // Decrypts an encrypted UserNameIdentityToken password with the server
  // private key (RSA-OAEP). Null when the server has no certificate; an
  // encrypted token is then rejected.
  std::function<StatusOr<ByteString>(
      std::span<const std::uint8_t>)>
      decrypt_user_token;
  std::function<base::Time()> now = &base::Time::Now;
  base::TimeDelta default_timeout = base::TimeDelta::FromMinutes(10);
  base::TimeDelta min_timeout = base::TimeDelta::FromSeconds(30);
  base::TimeDelta max_timeout = base::TimeDelta::FromHours(1);
  NamespaceIndex session_namespace_index = 2;
  NamespaceIndex token_namespace_index = 3;
};

class ServerSessionManager : private ServerSessionManagerContext {
 public:
  explicit ServerSessionManager(ServerSessionManagerContext&& context);

  [[nodiscard]] Awaitable<CreateSessionResponse> CreateSession(
      CreateSessionRequest request = {});
  [[nodiscard]] Awaitable<ActivateSessionResponse> ActivateSession(
      ActivateSessionRequest request);
  [[nodiscard]] CloseSessionResponse CloseSession(CloseSessionRequest request);

  void DetachSession(const NodeId& authentication_token);
  void PruneExpiredSessions();

  [[nodiscard]] std::optional<ServerSessionLookupResult> FindSession(
      const NodeId& authentication_token) const;

 private:
  struct SessionState {
    NodeId session_id;
    NodeId authentication_token;
    ByteString server_nonce;
    // Client application instance certificate (DER) captured at CreateSession.
    // Non-empty marks a secured session whose ActivateSession clientSignature
    // must verify.
    ByteString client_certificate;
    base::TimeDelta revised_timeout;
    base::Time expires_at;
    ServiceContext service_context;
    std::optional<AuthenticationResult> authentication_result;
    bool activated = false;
    bool attached = false;
  };

  [[nodiscard]] base::Time Now() const { return now(); }
  [[nodiscard]] base::TimeDelta ReviseTimeout(base::TimeDelta requested) const;
  [[nodiscard]] NodeId MakeSessionId();
  [[nodiscard]] NodeId MakeAuthenticationToken();
  [[nodiscard]] ByteString MakeServerNonce() const;
  [[nodiscard]] SessionState* FindSessionState(
      const NodeId& authentication_token);
  [[nodiscard]] const SessionState* FindSessionState(
      const NodeId& authentication_token) const;
  [[nodiscard]] bool RemoveSessionByUser(const NodeId& user_id);
  [[nodiscard]] bool HasSessionForUser(const NodeId& user_id) const;
  void RemoveSessionByToken(const NodeId& authentication_token);

  std::unordered_map<NodeId, SessionState> sessions_;
  UInt32 next_session_id_ = 1;
  UInt32 next_token_id_ = 1;
};

}  // namespace opcua
