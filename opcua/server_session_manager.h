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
  scada::ByteString client_certificate;
  scada::ByteString client_nonce;
  // SecureChannel binding, filled by the runtime from the connection (not on
  // the wire). When `channel_secure` is set the server requires the body's
  // `client_certificate` to match `channel_certificate` (the certificate the
  // SecureChannel already validated), guarding against a client presenting a
  // different certificate at the session layer than at the channel layer.
  bool channel_secure = false;
  scada::ByteString channel_certificate;
};

struct CreateSessionResponse {
  scada::Status status{scada::StatusCode::Good};
  scada::NodeId session_id;
  scada::NodeId authentication_token;
  scada::ByteString server_nonce;
  // Server application instance certificate (DER). The client signs
  // (server_certificate || server_nonce) in ActivateSession.
  scada::ByteString server_certificate;
  base::TimeDelta revised_timeout;
};

struct ActivateSessionRequest {
  scada::NodeId session_id;
  scada::NodeId authentication_token;
  std::optional<scada::LocalizedText> user_name;
  std::optional<scada::LocalizedText> password;
  bool delete_existing = false;
  bool allow_anonymous = false;
  // clientSignature (SignatureData): the client's signature over
  // (server_certificate || server_nonce) using the SecureChannel's asymmetric
  // signature algorithm. Empty under SecurityPolicy=None.
  std::string client_signature_algorithm;
  scada::ByteString client_signature;
  // Encrypted UserNameIdentityToken password. When
  // `password_encryption_algorithm` is non-empty the password is not in
  // `password` above but here as RSA-OAEP ciphertext of
  // [length(UInt32 LE) || password || server_nonce] under the server
  // certificate (OPC UA Part 4 §7.36). The manager decrypts it.
  scada::ByteString encrypted_password;
  std::string password_encryption_algorithm;
};

struct ActivateSessionResponse {
  scada::Status status{scada::StatusCode::Good};
  scada::ServiceContext service_context;
  std::optional<scada::AuthenticationResult> authentication_result;
  bool resumed = false;
};

struct CloseSessionRequest {
  scada::NodeId session_id;
  scada::NodeId authentication_token;
};

struct CloseSessionResponse {
  scada::Status status{scada::StatusCode::Good};
};

struct ServerSessionLookupResult {
  scada::NodeId session_id;
  scada::NodeId authentication_token;
  scada::ServiceContext service_context;
  std::optional<scada::AuthenticationResult> authentication_result;
  bool attached = false;
  bool activated = false;
};

struct ServerSessionManagerContext {
  std::shared_ptr<scada::CoroutineAuthenticator> authenticator;
  // Server application instance certificate (DER). When non-empty, the client
  // signs (server_certificate || server_nonce) and the manager verifies that
  // clientSignature in ActivateSession. Empty under SecurityPolicy=None.
  scada::ByteString server_certificate;
  // Decrypts an encrypted UserNameIdentityToken password with the server
  // private key (RSA-OAEP). Null when the server has no certificate; an
  // encrypted token is then rejected.
  std::function<scada::StatusOr<scada::ByteString>(
      std::span<const std::uint8_t>)>
      decrypt_user_token;
  std::function<base::Time()> now = &base::Time::Now;
  base::TimeDelta default_timeout = base::TimeDelta::FromMinutes(10);
  base::TimeDelta min_timeout = base::TimeDelta::FromSeconds(30);
  base::TimeDelta max_timeout = base::TimeDelta::FromHours(1);
  scada::NamespaceIndex session_namespace_index = 2;
  scada::NamespaceIndex token_namespace_index = 3;
};

class ServerSessionManager : private ServerSessionManagerContext {
 public:
  explicit ServerSessionManager(ServerSessionManagerContext&& context);

  [[nodiscard]] Awaitable<CreateSessionResponse> CreateSession(
      CreateSessionRequest request = {});
  [[nodiscard]] Awaitable<ActivateSessionResponse> ActivateSession(
      ActivateSessionRequest request);
  [[nodiscard]] CloseSessionResponse CloseSession(CloseSessionRequest request);

  void DetachSession(const scada::NodeId& authentication_token);
  void PruneExpiredSessions();

  [[nodiscard]] std::optional<ServerSessionLookupResult> FindSession(
      const scada::NodeId& authentication_token) const;

 private:
  struct SessionState {
    scada::NodeId session_id;
    scada::NodeId authentication_token;
    scada::ByteString server_nonce;
    // Client application instance certificate (DER) captured at CreateSession.
    // Non-empty marks a secured session whose ActivateSession clientSignature
    // must verify.
    scada::ByteString client_certificate;
    base::TimeDelta revised_timeout;
    base::Time expires_at;
    scada::ServiceContext service_context;
    std::optional<scada::AuthenticationResult> authentication_result;
    bool activated = false;
    bool attached = false;
  };

  [[nodiscard]] base::Time Now() const { return now(); }
  [[nodiscard]] base::TimeDelta ReviseTimeout(base::TimeDelta requested) const;
  [[nodiscard]] scada::NodeId MakeSessionId();
  [[nodiscard]] scada::NodeId MakeAuthenticationToken();
  [[nodiscard]] scada::ByteString MakeServerNonce() const;
  [[nodiscard]] SessionState* FindSessionState(
      const scada::NodeId& authentication_token);
  [[nodiscard]] const SessionState* FindSessionState(
      const scada::NodeId& authentication_token) const;
  [[nodiscard]] bool RemoveSessionByUser(const scada::NodeId& user_id);
  [[nodiscard]] bool HasSessionForUser(const scada::NodeId& user_id) const;
  void RemoveSessionByToken(const scada::NodeId& authentication_token);

  std::unordered_map<scada::NodeId, SessionState> sessions_;
  scada::UInt32 next_session_id_ = 1;
  scada::UInt32 next_token_id_ = 1;
};

}  // namespace opcua
