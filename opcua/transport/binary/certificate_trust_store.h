#pragma once

#include "opcua/scada/status.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>

// OpenSSL forward declaration to keep this header lean.
struct x509_store_st;
using X509_STORE = x509_store_st;

namespace opcua::binary {

// Configuration for a file-backed certificate trust store.
struct CertificateTrustStoreConfig {
  // Directory of trusted client application instance certificates (PEM or DER).
  // A client certificate is accepted if its SHA-1 thumbprint matches one of
  // these (explicit leaf trust).
  std::filesystem::path trusted_dir;
  // Optional directory of trusted issuer (CA) certificates. A client
  // certificate that is not explicitly trusted is accepted if it chains to one
  // of these issuers.
  std::filesystem::path issuer_dir;
  // Optional directory of CRLs (PEM or DER). When present, chain validation
  // additionally rejects certificates revoked by these CRLs.
  std::filesystem::path crl_dir;
  // Optional directory; rejected client certificates are copied here (DER) for
  // an operator to inspect and promote to the trusted store.
  std::filesystem::path rejected_dir;
};

// Validates client application instance certificates against a file-backed
// trust store. A certificate is accepted if it is within its validity period
// and either (a) its thumbprint is in the trusted leaf directory, or (b) it
// chains to a trusted issuer (and is not revoked by a configured CRL).
class CertificateTrustStore {
 public:
  explicit CertificateTrustStore(CertificateTrustStoreConfig config);
  ~CertificateTrustStore();

  CertificateTrustStore(const CertificateTrustStore&) = delete;
  CertificateTrustStore& operator=(const CertificateTrustStore&) = delete;

  // Returns Good if the DER-encoded certificate parses, is within its validity
  // period, and is explicitly trusted or chains to a trusted issuer (not
  // revoked). Otherwise writes the certificate to the rejected directory (when
  // configured) and returns a bad Status. Suitable as
  // SecureChannelServerConfig::validate_client_certificate.
  [[nodiscard]] Status Validate(
      std::span<const std::uint8_t> certificate_der) const;

  // Number of trusted leaf certificates loaded (for diagnostics / tests).
  [[nodiscard]] std::size_t trusted_count() const {
    return trusted_thumbprints_.size();
  }

 private:
  void LoadTrusted();
  void LoadIssuerStore();
  [[nodiscard]] bool ChainVerifies(void* x509_cert) const;
  void WriteRejected(std::span<const std::uint8_t> certificate_der,
                     const std::string& thumbprint_hex) const;

  std::filesystem::path trusted_dir_;
  std::filesystem::path issuer_dir_;
  std::filesystem::path crl_dir_;
  std::filesystem::path rejected_dir_;
  std::unordered_set<std::string> trusted_thumbprints_;  // lowercase hex
  X509_STORE* issuer_store_ = nullptr;  // null when no issuer_dir configured
};

}  // namespace opcua::binary
