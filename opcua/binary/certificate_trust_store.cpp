#include "opcua/binary/certificate_trust_store.h"

#include "opcua/base/boost_log.h"
#include "opcua/binary/crypto.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace opcua::binary {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaCertificateTrustStore")};

std::string ToHex(const scada::ByteString& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<std::uint8_t>(byte);
    hex.push_back(kDigits[value >> 4]);
    hex.push_back(kDigits[value & 0x0f]);
  }
  return hex;
}

std::optional<std::vector<char>> ReadFileBytes(
    const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::nullopt;
  }
  return std::vector<char>{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
}

// Loads a certificate from raw file bytes, accepting either PEM or DER.
scada::StatusOr<crypto::Certificate> LoadAnyCertificate(
    const std::vector<char>& bytes) {
  const std::string_view text{bytes.data(), bytes.size()};
  if (text.find("BEGIN CERTIFICATE") != std::string_view::npos) {
    return crypto::LoadPemCertificate(text);
  }
  return crypto::LoadDerCertificate(
      {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

// Loads a CRL from raw file bytes, accepting either PEM or DER. Returns an
// owning X509_CRL* (caller frees) or nullptr.
X509_CRL* LoadCrl(const std::vector<char>& bytes) {
  const std::string_view text{bytes.data(), bytes.size()};
  if (text.find("BEGIN X509 CRL") != std::string_view::npos) {
    BIO* bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
    if (!bio) {
      return nullptr;
    }
    X509_CRL* crl = PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return crl;
  }
  const unsigned char* cursor =
      reinterpret_cast<const unsigned char*>(bytes.data());
  return d2i_X509_CRL(nullptr, &cursor, static_cast<long>(bytes.size()));
}

}  // namespace

CertificateTrustStore::CertificateTrustStore(CertificateTrustStoreConfig config)
    : trusted_dir_{std::move(config.trusted_dir)},
      issuer_dir_{std::move(config.issuer_dir)},
      crl_dir_{std::move(config.crl_dir)},
      rejected_dir_{std::move(config.rejected_dir)} {
  LoadTrusted();
  LoadIssuerStore();
}

CertificateTrustStore::~CertificateTrustStore() {
  if (issuer_store_) {
    X509_STORE_free(issuer_store_);
  }
}

void CertificateTrustStore::LoadTrusted() {
  std::error_code ec;
  if (trusted_dir_.empty() ||
      !std::filesystem::is_directory(trusted_dir_, ec)) {
    LOG_WARNING(logger_) << "OPC UA trusted certificate directory missing"
                         << LOG_TAG("Path", trusted_dir_.string());
    return;
  }

  for (const auto& entry :
       std::filesystem::directory_iterator{trusted_dir_, ec}) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    auto bytes = ReadFileBytes(entry.path());
    if (!bytes.has_value()) {
      continue;
    }
    auto certificate = LoadAnyCertificate(*bytes);
    if (!certificate.ok()) {
      LOG_WARNING(logger_) << "OPC UA trusted certificate failed to parse"
                           << LOG_TAG("Path", entry.path().string());
      continue;
    }
    auto thumbprint = crypto::CertificateThumbprint(*certificate);
    if (!thumbprint.ok()) {
      continue;
    }
    trusted_thumbprints_.insert(ToHex(*thumbprint));
  }

  LOG_INFO(logger_) << "OPC UA trusted certificate store loaded"
                    << LOG_TAG("Path", trusted_dir_.string())
                    << LOG_TAG("Count", trusted_thumbprints_.size());
}

void CertificateTrustStore::LoadIssuerStore() {
  std::error_code ec;
  if (issuer_dir_.empty() || !std::filesystem::is_directory(issuer_dir_, ec)) {
    return;
  }

  issuer_store_ = X509_STORE_new();
  if (!issuer_store_) {
    return;
  }

  std::size_t ca_count = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator{issuer_dir_, ec}) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    auto bytes = ReadFileBytes(entry.path());
    if (!bytes.has_value()) {
      continue;
    }
    auto certificate = LoadAnyCertificate(*bytes);
    if (!certificate.ok()) {
      continue;
    }
    // X509_STORE_add_cert takes its own reference, so the crypto::Certificate
    // can drop ours when it goes out of scope.
    if (X509_STORE_add_cert(issuer_store_, certificate->raw()) == 1) {
      ++ca_count;
    }
  }

  std::size_t crl_count = 0;
  if (!crl_dir_.empty() && std::filesystem::is_directory(crl_dir_, ec)) {
    for (const auto& entry :
         std::filesystem::directory_iterator{crl_dir_, ec}) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      auto bytes = ReadFileBytes(entry.path());
      if (!bytes.has_value()) {
        continue;
      }
      X509_CRL* crl = LoadCrl(*bytes);
      if (!crl) {
        continue;
      }
      if (X509_STORE_add_crl(issuer_store_, crl) == 1) {
        ++crl_count;
      }
      X509_CRL_free(crl);  // the store took its own reference
    }
    if (crl_count > 0) {
      X509_STORE_set_flags(issuer_store_,
                           X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
    }
  }

  if (ca_count == 0) {
    X509_STORE_free(issuer_store_);
    issuer_store_ = nullptr;
  }

  LOG_INFO(logger_) << "OPC UA issuer certificate store loaded"
                    << LOG_TAG("Path", issuer_dir_.string())
                    << LOG_TAG("Issuers", ca_count)
                    << LOG_TAG("Crls", crl_count);
}

bool CertificateTrustStore::ChainVerifies(void* x509_cert) const {
  if (!issuer_store_) {
    return false;
  }
  X509_STORE_CTX* ctx = X509_STORE_CTX_new();
  if (!ctx) {
    return false;
  }
  bool ok = false;
  if (X509_STORE_CTX_init(ctx, issuer_store_, static_cast<X509*>(x509_cert),
                          nullptr) == 1) {
    ok = X509_verify_cert(ctx) == 1;
    if (!ok) {
      LOG_WARNING(logger_)
          << "OPC UA client certificate chain verification failed"
          << LOG_TAG("Error",
                     X509_verify_cert_error_string(
                         X509_STORE_CTX_get_error(ctx)));
    }
  }
  X509_STORE_CTX_free(ctx);
  return ok;
}

void CertificateTrustStore::WriteRejected(
    std::span<const std::uint8_t> certificate_der,
    const std::string& thumbprint_hex) const {
  if (rejected_dir_.empty() || thumbprint_hex.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(rejected_dir_, ec);
  const auto path = rejected_dir_ / (thumbprint_hex + ".der");
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!stream) {
    return;
  }
  stream.write(reinterpret_cast<const char*>(certificate_der.data()),
               static_cast<std::streamsize>(certificate_der.size()));
}

scada::Status CertificateTrustStore::Validate(
    std::span<const std::uint8_t> certificate_der) const {
  auto certificate = crypto::LoadDerCertificate(certificate_der);
  if (!certificate.ok()) {
    return scada::Status{scada::StatusCode::Bad};
  }

  auto thumbprint = crypto::CertificateThumbprint(*certificate);
  const std::string thumbprint_hex = thumbprint.ok() ? ToHex(*thumbprint) : "";

  if (!crypto::CertificateTimeValid(*certificate)) {
    LOG_WARNING(logger_) << "OPC UA client certificate rejected"
                         << LOG_TAG("Reason", "OutsideValidityPeriod")
                         << LOG_TAG("Thumbprint", thumbprint_hex);
    WriteRejected(certificate_der, thumbprint_hex);
    return scada::Status{scada::StatusCode::Bad};
  }

  // Explicit leaf trust.
  if (!thumbprint_hex.empty() &&
      trusted_thumbprints_.contains(thumbprint_hex)) {
    return scada::Status{scada::StatusCode::Good};
  }

  // Issuer-chain trust (with CRL revocation when configured).
  if (ChainVerifies(certificate->raw())) {
    return scada::Status{scada::StatusCode::Good};
  }

  LOG_WARNING(logger_) << "OPC UA client certificate rejected"
                       << LOG_TAG("Reason", "Untrusted")
                       << LOG_TAG("Thumbprint", thumbprint_hex);
  WriteRejected(certificate_der, thumbprint_hex);
  return scada::Status{scada::StatusCode::Bad};
}

}  // namespace opcua::binary
