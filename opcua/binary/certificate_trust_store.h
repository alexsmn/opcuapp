#pragma once

#include "opcua/scada/status.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>

namespace opcua::binary {

// Configuration for a file-backed certificate trust store.
struct CertificateTrustStoreConfig {
  // Directory of trusted client application instance certificates (PEM or DER).
  // A client certificate is accepted only if its SHA-1 thumbprint matches one
  // of these.
  std::filesystem::path trusted_dir;
  // Optional directory; rejected client certificates are copied here (DER) for
  // an operator to inspect and promote to the trusted store.
  std::filesystem::path rejected_dir;
};

// Validates client application instance certificates against a directory of
// trusted certificates. This is a minimal trusted-list store: it enforces the
// certificate's validity period and trusted-thumbprint membership. Issuer-chain
// and CRL/OCSP revocation checking are not yet implemented.
class CertificateTrustStore {
 public:
  explicit CertificateTrustStore(CertificateTrustStoreConfig config);

  // Returns Good if the DER-encoded certificate parses, is within its validity
  // period, and its thumbprint is in the trusted set. Otherwise writes the
  // certificate to the rejected directory (when configured) and returns a bad
  // Status. Suitable as SecureChannelServerConfig::validate_client_certificate.
  [[nodiscard]] scada::Status Validate(
      std::span<const std::uint8_t> certificate_der) const;

  // Number of trusted certificates loaded (for diagnostics / tests).
  [[nodiscard]] std::size_t trusted_count() const {
    return trusted_thumbprints_.size();
  }

 private:
  void LoadTrusted();
  void WriteRejected(std::span<const std::uint8_t> certificate_der,
                     const std::string& thumbprint_hex) const;

  std::filesystem::path trusted_dir_;
  std::filesystem::path rejected_dir_;
  std::unordered_set<std::string> trusted_thumbprints_;  // lowercase hex
};

}  // namespace opcua::binary
