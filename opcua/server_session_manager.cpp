#include "opcua/server_session_manager.h"

#include "opcua/base/boost_log.h"
#include "opcua/binary/crypto.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/status_or.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

namespace opcua {

namespace {

BoostLogger logger_{LOG_NAME("ServerSessionManager")};

scada::Status SessionMissingStatus() {
  return scada::StatusCode::Bad_SessionIsLoggedOff;
}

std::span<const std::uint8_t> ByteSpan(const scada::ByteString& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// Verifies the ActivateSession clientSignature: an RSA-PKCS#1-SHA256 signature
// over (serverCertificate || serverNonce) made with the private key of the
// client application instance certificate (OPC UA Part 4 §5.6.3).
bool VerifyApplicationSignature(const scada::ByteString& client_certificate_der,
                                const scada::ByteString& server_certificate,
                                const scada::ByteString& server_nonce,
                                const scada::ByteString& signature) {
  auto certificate =
      binary::crypto::LoadDerCertificate(ByteSpan(client_certificate_der));
  if (!certificate.ok()) {
    return false;
  }
  auto public_key = binary::crypto::CertificatePublicKey(*certificate);
  if (!public_key.ok()) {
    return false;
  }
  scada::ByteString data;
  data.reserve(server_certificate.size() + server_nonce.size());
  data.insert(data.end(), server_certificate.begin(), server_certificate.end());
  data.insert(data.end(), server_nonce.begin(), server_nonce.end());
  return binary::crypto::RsaPkcs1Sha256Verify(*public_key, ByteSpan(data),
                                              ByteSpan(signature));
}

// Recovers the cleartext password from an encrypted UserNameIdentityToken.
// The decrypted secret is [length(UInt32 LE) || password || serverNonce]; the
// trailing serverNonce must match the session's nonce (OPC UA Part 4 §7.36).
scada::StatusOr<std::string> RecoverEncryptedPassword(
    const std::function<scada::StatusOr<scada::ByteString>(
        std::span<const std::uint8_t>)>& decrypt_user_token,
    const scada::ByteString& ciphertext,
    const scada::ByteString& server_nonce) {
  if (!decrypt_user_token) {
    return scada::StatusOr<std::string>{
        scada::Status{scada::StatusCode::Bad_WrongLoginCredentials}};
  }
  auto plaintext = decrypt_user_token(ByteSpan(ciphertext));
  if (!plaintext.ok()) {
    return scada::StatusOr<std::string>{
        scada::Status{scada::StatusCode::Bad_WrongLoginCredentials}};
  }
  if (plaintext->size() < 4) {
    return scada::StatusOr<std::string>{
        scada::Status{scada::StatusCode::Bad_WrongLoginCredentials}};
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(plaintext->data());
  const std::uint32_t length = static_cast<std::uint32_t>(bytes[0]) |
                               (static_cast<std::uint32_t>(bytes[1]) << 8) |
                               (static_cast<std::uint32_t>(bytes[2]) << 16) |
                               (static_cast<std::uint32_t>(bytes[3]) << 24);
  if (length > plaintext->size() - 4 || length < server_nonce.size()) {
    return scada::StatusOr<std::string>{
        scada::Status{scada::StatusCode::Bad_WrongLoginCredentials}};
  }
  const std::size_t password_len = length - server_nonce.size();
  const auto secret_begin = plaintext->begin() + 4;
  if (!std::equal(server_nonce.begin(), server_nonce.end(),
                  secret_begin + static_cast<std::ptrdiff_t>(password_len))) {
    return scada::StatusOr<std::string>{
        scada::Status{scada::StatusCode::Bad_WrongLoginCredentials}};
  }
  return scada::StatusOr<std::string>{
      std::string{secret_begin,
                  secret_begin + static_cast<std::ptrdiff_t>(password_len)}};
}

}  // namespace

ServerSessionManager::ServerSessionManager(
    ServerSessionManagerContext&& context)
    : ServerSessionManagerContext{std::move(context)} {}

Awaitable<CreateSessionResponse> ServerSessionManager::CreateSession(
    CreateSessionRequest request) {
  PruneExpiredSessions();

  // SecureChannel binding: a secured session must present, at the session
  // layer, the same client application instance certificate the SecureChannel
  // already validated (OPC UA Part 4 §5.6.2).
  if (request.channel_secure) {
    if (request.client_certificate.empty() ||
        request.client_certificate != request.channel_certificate) {
      LOG_WARNING(logger_) << "OPC UA session creation rejected"
                           << LOG_TAG("Reason", "ClientCertificateMismatch");
      co_return CreateSessionResponse{
          .status = scada::StatusCode::Bad_ApplicationSignatureInvalid};
    }
  }

  const auto revised_timeout = ReviseTimeout(request.requested_timeout);
  const auto session_id = MakeSessionId();
  const auto authentication_token = MakeAuthenticationToken();

  SessionState session{
      .session_id = session_id,
      .authentication_token = authentication_token,
      .server_nonce = MakeServerNonce(),
      .client_certificate = request.client_certificate,
      .revised_timeout = revised_timeout,
      .expires_at = Now() + revised_timeout,
  };

  auto server_nonce = session.server_nonce;
  sessions_.insert_or_assign(authentication_token, std::move(session));

  LOG_INFO(logger_) << "OPC UA session created"
                    << LOG_TAG("SessionId", session_id.ToString())
                    << LOG_TAG("AuthenticationToken",
                               authentication_token.ToString())
                    << LOG_TAG("RequestedTimeoutMs",
                               request.requested_timeout.InMilliseconds())
                    << LOG_TAG("RevisedTimeoutMs",
                               revised_timeout.InMilliseconds());

  co_return CreateSessionResponse{
      .status = scada::StatusCode::Good,
      .session_id = session_id,
      .authentication_token = authentication_token,
      .server_nonce = std::move(server_nonce),
      .server_certificate = server_certificate,
      .revised_timeout = revised_timeout,
  };
}

Awaitable<ActivateSessionResponse> ServerSessionManager::ActivateSession(
    ActivateSessionRequest request) {
  PruneExpiredSessions();

  auto session_it = sessions_.find(request.authentication_token);
  if (session_it == sessions_.end())
    co_return ActivateSessionResponse{SessionMissingStatus()};
  // cppcheck-suppress derefInvalidIteratorRedundantCheck
  auto& session = session_it->second;
  if (session.session_id != request.session_id) {
    LOG_WARNING(logger_) << "OPC UA session activation failed"
                         << LOG_TAG("Reason", "SessionMismatch")
                         << LOG_TAG("SessionId", request.session_id.ToString())
                         << LOG_TAG("AuthenticationToken",
                                    request.authentication_token.ToString());
    co_return ActivateSessionResponse{SessionMissingStatus()};
  }

  // Verify the client application signature for a secured session before any
  // user authentication or session resume (OPC UA Part 4 §5.6.3).
  if (!session.client_certificate.empty()) {
    if (request.client_signature.empty() ||
        !VerifyApplicationSignature(session.client_certificate,
                                    server_certificate, session.server_nonce,
                                    request.client_signature)) {
      LOG_WARNING(logger_) << "OPC UA session activation failed"
                           << LOG_TAG("Reason", "ApplicationSignatureInvalid")
                           << LOG_TAG("SessionId",
                                      request.session_id.ToString());
      co_return ActivateSessionResponse{
          scada::StatusCode::Bad_ApplicationSignatureInvalid};
    }
  }

  if (session.activated) {
    session.attached = true;
    session.expires_at = Now() + session.revised_timeout;
    LOG_INFO(logger_) << "OPC UA session resumed"
                      << LOG_TAG("SessionId", session.session_id.ToString())
                      << LOG_TAG("AuthenticationToken",
                                 session.authentication_token.ToString())
                      << LOG_TAG("Attached", session.attached);
    co_return ActivateSessionResponse{
        .status = scada::StatusCode::Good,
        .service_context = session.service_context,
        .authentication_result = session.authentication_result,
        .resumed = true,
    };
  }

  std::optional<scada::AuthenticationResult> auth_result;
  if (!request.allow_anonymous) {
    // Decrypt an encrypted UserNameIdentityToken password before checking
    // credentials.
    if (!request.password_encryption_algorithm.empty()) {
      auto password = RecoverEncryptedPassword(
          decrypt_user_token, request.encrypted_password, session.server_nonce);
      if (!password.ok()) {
        LOG_WARNING(logger_) << "OPC UA session activation failed"
                             << LOG_TAG("Reason", "UserTokenDecryptFailed")
                             << LOG_TAG("SessionId",
                                        request.session_id.ToString());
        co_return ActivateSessionResponse{password.status()};
      }
      request.password = scada::ToLocalizedText(*password);
    }
    if (!request.user_name.has_value() || !request.password.has_value()) {
      LOG_WARNING(logger_) << "OPC UA session activation failed"
                           << LOG_TAG("Reason", "MissingCredentials")
                           << LOG_TAG("SessionId", request.session_id.ToString())
                           << LOG_TAG("AuthenticationToken",
                                      request.authentication_token.ToString());
      co_return ActivateSessionResponse{
          scada::StatusCode::Bad_WrongLoginCredentials};
    }

    auto auth = co_await authenticator->Authenticate(
        std::move(*request.user_name), std::move(*request.password));
    if (!auth.ok()) {
      LOG_WARNING(logger_) << "OPC UA session activation failed"
                           << LOG_TAG("Reason", "AuthenticationFailed")
                           << LOG_TAG("SessionId", request.session_id.ToString())
                           << LOG_TAG("AuthenticationToken",
                                      request.authentication_token.ToString());
      co_return ActivateSessionResponse{auth.status()};
    }

    auth_result = *auth;
    if (!auth_result->multi_sessions) {
      if (HasSessionForUser(auth_result->user_id)) {
        if (!request.delete_existing) {
          LOG_WARNING(logger_) << "OPC UA session activation failed"
                               << LOG_TAG("Reason", "UserAlreadyLoggedOn")
                               << LOG_TAG("SessionId", request.session_id.ToString())
                               << LOG_TAG("AuthenticationToken",
                                          request.authentication_token.ToString())
                               << LOG_TAG("UserId",
                                          auth_result->user_id.ToString());
          co_return ActivateSessionResponse{
              scada::StatusCode::Bad_UserIsAlreadyLoggedOn};
        }
        [[maybe_unused]] const auto removed =
            RemoveSessionByUser(auth_result->user_id);
      }
    }
  }

  session_it = sessions_.find(request.authentication_token);
  if (session_it == sessions_.end())
    co_return ActivateSessionResponse{SessionMissingStatus()};
  // cppcheck-suppress derefInvalidIteratorRedundantCheck
  auto& refreshed_session = session_it->second;
  if (refreshed_session.session_id != request.session_id) {
    LOG_WARNING(logger_) << "OPC UA session activation failed"
                         << LOG_TAG("Reason", "SessionMismatchAfterAuth")
                         << LOG_TAG("SessionId", request.session_id.ToString())
                         << LOG_TAG("AuthenticationToken",
                                    request.authentication_token.ToString());
    co_return ActivateSessionResponse{SessionMissingStatus()};
  }

  if (auth_result.has_value()) {
    refreshed_session.authentication_result = auth_result;
    refreshed_session.service_context =
        scada::ServiceContext{}.with_user_id(auth_result->user_id);
  } else {
    refreshed_session.service_context = scada::ServiceContext{};
  }
  refreshed_session.activated = true;
  refreshed_session.attached = true;
  refreshed_session.expires_at = Now() + refreshed_session.revised_timeout;

  if (auth_result.has_value()) {
    LOG_INFO(logger_) << "OPC UA session activated"
                      << LOG_TAG("SessionId",
                                 refreshed_session.session_id.ToString())
                      << LOG_TAG("AuthenticationToken",
                                 refreshed_session.authentication_token.ToString())
                      << LOG_TAG("UserId",
                                 auth_result->user_id.ToString())
                      << LOG_TAG("MultiSessions",
                                 auth_result->multi_sessions);
  } else {
    LOG_INFO(logger_) << "OPC UA anonymous session activated"
                      << LOG_TAG("SessionId",
                                 refreshed_session.session_id.ToString())
                      << LOG_TAG("AuthenticationToken",
                                 refreshed_session.authentication_token.ToString());
  }

  co_return ActivateSessionResponse{
      .status = scada::StatusCode::Good,
      .service_context = refreshed_session.service_context,
      .authentication_result = refreshed_session.authentication_result,
      .resumed = false,
  };
}

CloseSessionResponse ServerSessionManager::CloseSession(
    CloseSessionRequest request) {
  auto* session = FindSessionState(request.authentication_token);
  if (!session || session->session_id != request.session_id) {
    LOG_WARNING(logger_) << "OPC UA session close failed"
                         << LOG_TAG("Reason", "SessionMissing")
                         << LOG_TAG("SessionId", request.session_id.ToString())
                         << LOG_TAG("AuthenticationToken",
                                    request.authentication_token.ToString());
    return {.status = SessionMissingStatus()};
  }

  LOG_INFO(logger_) << "OPC UA session closed"
                    << LOG_TAG("SessionId", session->session_id.ToString())
                    << LOG_TAG("AuthenticationToken",
                               session->authentication_token.ToString())
                    << LOG_TAG("Attached", session->attached);

  RemoveSessionByToken(request.authentication_token);
  return {.status = scada::StatusCode::Good};
}

void ServerSessionManager::DetachSession(
    const scada::NodeId& authentication_token) {
  if (auto* session = FindSessionState(authentication_token)) {
    session->attached = false;
    LOG_INFO(logger_) << "OPC UA session detached"
                      << LOG_TAG("SessionId", session->session_id.ToString())
                      << LOG_TAG("AuthenticationToken",
                                 session->authentication_token.ToString());
  }
}

void ServerSessionManager::PruneExpiredSessions() {
  const auto now_time = Now();
  std::erase_if(sessions_, [now_time](const auto& entry) {
    const auto expired = entry.second.expires_at <= now_time;
    if (expired) {
      LOG_INFO(logger_) << "OPC UA session expired"
                        << LOG_TAG("SessionId", entry.second.session_id.ToString())
                        << LOG_TAG("AuthenticationToken",
                                   entry.second.authentication_token.ToString())
                        << LOG_TAG("Activated", entry.second.activated)
                        << LOG_TAG("Attached", entry.second.attached);
    }
    return expired;
  });
}

std::optional<ServerSessionLookupResult> ServerSessionManager::FindSession(
    const scada::NodeId& authentication_token) const {
  const auto* session = FindSessionState(authentication_token);
  if (!session)
    return std::nullopt;

  return ServerSessionLookupResult{
      .session_id = session->session_id,
      .authentication_token = session->authentication_token,
      .service_context = session->service_context,
      .authentication_result = session->authentication_result,
      .attached = session->attached,
      .activated = session->activated,
  };
}

base::TimeDelta ServerSessionManager::ReviseTimeout(
    base::TimeDelta requested) const {
  if (requested.is_zero())
    return default_timeout;
  return std::clamp(requested, min_timeout, max_timeout);
}

scada::NodeId ServerSessionManager::MakeSessionId() {
  return {next_session_id_++, session_namespace_index};
}

scada::NodeId ServerSessionManager::MakeAuthenticationToken() {
  return {next_token_id_++, token_namespace_index};
}

scada::ByteString ServerSessionManager::MakeServerNonce() const {
  // OPC UA Part 4 §5.6.2 requires the server nonce to be a cryptographically
  // random value of at least the active SecurityPolicy's nonce length (32
  // bytes for Basic256Sha256). The client signs (serverCertificate ||
  // serverNonce) in ActivateSession, so a predictable nonce would let an
  // attacker forge that signature.
  constexpr std::size_t kServerNonceLength = 32;
  auto nonce = binary::crypto::GenerateNonce(kServerNonceLength);
  if (!nonce.ok()) {
    LOG_WARNING(logger_) << "OPC UA server nonce generation failed";
    return scada::ByteString(kServerNonceLength, 0);
  }
  return std::move(*nonce);
}

ServerSessionManager::SessionState* ServerSessionManager::FindSessionState(
    const scada::NodeId& authentication_token) {
  auto it = sessions_.find(authentication_token);
  return it != sessions_.end() ? &it->second : nullptr;
}

const ServerSessionManager::SessionState*
ServerSessionManager::FindSessionState(
    const scada::NodeId& authentication_token) const {
  auto it = sessions_.find(authentication_token);
  return it != sessions_.end() ? &it->second : nullptr;
}

bool ServerSessionManager::RemoveSessionByUser(const scada::NodeId& user_id) {
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [&user_id](const auto& entry) {
                           return entry.second.authentication_result.has_value() &&
                                  entry.second.authentication_result->user_id ==
                                      user_id;
                         });
  if (it == sessions_.end())
    return false;

  LOG_INFO(logger_) << "OPC UA session removed for single-session user"
                    << LOG_TAG("SessionId", it->second.session_id.ToString())
                    << LOG_TAG("AuthenticationToken",
                               it->second.authentication_token.ToString())
                    << LOG_TAG("UserId", user_id.ToString());
  sessions_.erase(it);
  return true;
}

bool ServerSessionManager::HasSessionForUser(
    const scada::NodeId& user_id) const {
  return std::any_of(sessions_.begin(), sessions_.end(),
                     [&user_id](const auto& entry) {
                       return entry.second.authentication_result.has_value() &&
                              entry.second.authentication_result->user_id ==
                                  user_id;
                     });
}

void ServerSessionManager::RemoveSessionByToken(
    const scada::NodeId& authentication_token) {
  if (auto* session = FindSessionState(authentication_token)) {
    LOG_INFO(logger_) << "OPC UA session forgotten"
                      << LOG_TAG("SessionId", session->session_id.ToString())
                      << LOG_TAG("AuthenticationToken",
                                 session->authentication_token.ToString());
  }
  sessions_.erase(authentication_token);
}

}  // namespace opcua
