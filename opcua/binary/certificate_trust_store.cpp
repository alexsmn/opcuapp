#include "opcua/binary/certificate_trust_store.h"

#include "opcua/base/boost_log.h"
#include "opcua/binary/crypto.h"

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

}  // namespace

CertificateTrustStore::CertificateTrustStore(CertificateTrustStoreConfig config)
    : trusted_dir_{std::move(config.trusted_dir)},
      rejected_dir_{std::move(config.rejected_dir)} {
  LoadTrusted();
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

  if (thumbprint_hex.empty() ||
      !trusted_thumbprints_.contains(thumbprint_hex)) {
    LOG_WARNING(logger_) << "OPC UA client certificate rejected"
                         << LOG_TAG("Reason", "Untrusted")
                         << LOG_TAG("Thumbprint", thumbprint_hex);
    WriteRejected(certificate_der, thumbprint_hex);
    return scada::Status{scada::StatusCode::Bad};
  }

  return scada::Status{scada::StatusCode::Good};
}

}  // namespace opcua::binary
