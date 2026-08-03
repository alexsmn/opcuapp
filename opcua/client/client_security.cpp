#include "opcua/client/client_security.h"

#include "opcua/transport/binary/certificate_trust_store.h"
#include "opcua/transport/binary/crypto.h"

#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace opcua {

namespace {

std::optional<std::string> ReadFile(const std::string& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{stream},
                     std::istreambuf_iterator<char>{}};
}

}  // namespace

SecurityPreference ToSecurityPreference(
    const SessionSecuritySettings& settings) {
  SecurityPreference preference;
  switch (settings.mode) {
    case SessionSecuritySettings::Mode::None:
      preference.mode = SecurityPreference::Mode::None;
      break;
    case SessionSecuritySettings::Mode::Auto:
      preference.mode = settings.has_trust_store()
                            ? SecurityPreference::Mode::SignAndEncrypt
                            : SecurityPreference::Mode::Auto;
      break;
    case SessionSecuritySettings::Mode::SignAndEncrypt:
      preference.mode = SecurityPreference::Mode::SignAndEncrypt;
      break;
  }
  if (!settings.required_policy_uri.empty()) {
    preference.required_policy_uri = settings.required_policy_uri;
  }
  return preference;
}

ClientCapabilities CapabilitiesFor(const SessionSecuritySettings& settings) {
  const bool has_client_certificate =
      !settings.client_certificate_path.empty() &&
      !settings.client_private_key_path.empty();
  return has_client_certificate ? ClientCapabilities::Default()
                                : ClientCapabilities::NoneOnly();
}

StatusOr<binary::ClientSecureChannel::Security> BuildChannelSecurity(
    const EndpointDescription& endpoint,
    const SessionSecuritySettings& settings) {
  using Security = binary::ClientSecureChannel::Security;

  // An unsecured endpoint needs no certificates; the default Security
  // (SecurityPolicy=None) is what the single-argument channel uses anyway.
  if (endpoint.security_mode == MessageSecurityMode::None) {
    // Default-initialized, not `Security{}`: the aggregate holds a
    // crypto::Certificate whose default constructor is explicit, which makes
    // brace-init ill-formed in a copy-initialization context.
    Security unsecured;
    return StatusOr<Security>{std::move(unsecured)};
  }

  auto client_cert_pem = ReadFile(settings.client_certificate_path);
  auto client_key_pem = ReadFile(settings.client_private_key_path);
  if (!client_cert_pem || !client_key_pem) {
    return StatusOr<Security>{Status{StatusCode::Bad}};
  }
  auto client_certificate =
      binary::crypto::LoadPemCertificate(*client_cert_pem);
  if (!client_certificate.ok()) {
    return StatusOr<Security>{client_certificate.status()};
  }
  auto client_private_key = binary::crypto::LoadPemPrivateKey(*client_key_pem);
  if (!client_private_key.ok()) {
    return StatusOr<Security>{client_private_key.status()};
  }

  const auto& server_der = endpoint.server_certificate;
  const std::span<const std::uint8_t> server_der_span{
      reinterpret_cast<const std::uint8_t*>(server_der.data()),
      server_der.size()};

  // Authenticate the server before encrypting anything to it (see the header).
  if (settings.has_trust_store()) {
    binary::CertificateTrustStore trust_store{
        binary::CertificateTrustStoreConfig{
            .trusted_dir = settings.trusted_certificates_dir,
            .issuer_dir = settings.issuer_certificates_dir,
            .crl_dir = settings.crl_dir,
            .rejected_dir = settings.rejected_certificates_dir,
        }};
    if (const auto status = trust_store.Validate(server_der_span);
        status.bad()) {
      return StatusOr<Security>{status};
    }
  }

  auto server_certificate = binary::crypto::LoadDerCertificate(server_der_span);
  if (!server_certificate.ok()) {
    return StatusOr<Security>{server_certificate.status()};
  }

  Security security;
  security.security_policy_uri = endpoint.security_policy_uri;
  security.security_mode = static_cast<binary::MessageSecurityMode>(
      static_cast<UInt32>(endpoint.security_mode));
  security.client_certificate = std::move(*client_certificate);
  security.client_private_key = std::move(*client_private_key);
  security.server_certificate = std::move(*server_certificate);
  return StatusOr<Security>{std::move(security)};
}

bool SecurityEquivalent(const EndpointDescription& a,
                        const EndpointDescription& b) {
  if (a.security_mode != b.security_mode ||
      a.security_policy_uri != b.security_policy_uri ||
      a.server_certificate != b.server_certificate ||
      a.transport_profile_uri != b.transport_profile_uri ||
      a.user_identity_tokens.size() != b.user_identity_tokens.size()) {
    return false;
  }
  for (size_t i = 0; i < a.user_identity_tokens.size(); ++i) {
    const auto& lhs = a.user_identity_tokens[i];
    const auto& rhs = b.user_identity_tokens[i];
    if (lhs.policy_id != rhs.policy_id || lhs.token_type != rhs.token_type ||
        lhs.security_policy_uri != rhs.security_policy_uri) {
      return false;
    }
  }
  return true;
}

bool EndpointSetsSecurityEquivalent(
    std::span<const EndpointDescription> discovered,
    std::span<const EndpointDescription> authoritative) {
  // Nothing to check against — see the header for why this is not a failure.
  if (authoritative.empty()) {
    return true;
  }
  // No discovery ran, so there is no selection to re-check. Under
  // SessionSecuritySettings::Mode::None — the default, and what every link
  // without a "security" block uses — ClientSession skips GetEndpoints
  // entirely and connects with SecurityPolicy=None because it was configured
  // to, not because a discovery answer steered it there. This cannot be an
  // attacker suppressing discovery: in the modes that do discover, an empty
  // offered list fails endpoint selection before CreateSession is ever sent,
  // so an empty list here means the caller never asked.
  if (discovered.empty()) {
    return true;
  }
  if (discovered.size() != authoritative.size()) {
    return false;
  }
  // Quadratic, but these lists are a handful of entries per server and the
  // comparison runs once per connect. Matching by search rather than by index
  // keeps the check independent of ordering, which the spec does not fix.
  std::vector<bool> matched(authoritative.size(), false);
  for (const auto& endpoint : discovered) {
    bool found = false;
    for (size_t i = 0; i < authoritative.size(); ++i) {
      if (matched[i] || !SecurityEquivalent(endpoint, authoritative[i])) {
        continue;
      }
      matched[i] = true;
      found = true;
      break;
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

}  // namespace opcua
