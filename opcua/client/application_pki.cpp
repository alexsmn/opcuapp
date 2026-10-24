#include "opcua/client/application_pki.h"

#include "opcua/transport/binary/crypto.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace opcua {
namespace {

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream)
    return std::nullopt;
  return std::string{std::istreambuf_iterator<char>{stream},
                     std::istreambuf_iterator<char>{}};
}

// Loads PEM or DER, as CertificateTrustStore does.
std::optional<binary::crypto::Certificate> LoadCertificateFile(
    const std::filesystem::path& path) {
  auto bytes = ReadFile(path);
  if (!bytes)
    return std::nullopt;
  if (auto pem = binary::crypto::LoadPemCertificate(*bytes); pem.ok())
    return std::move(*pem);
  auto der = binary::crypto::LoadDerCertificate(
      {reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()});
  if (der.ok())
    return std::move(*der);
  return std::nullopt;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << text;
  return static_cast<bool>(stream);
}

}  // namespace

ApplicationPki::ApplicationPki(std::filesystem::path root)
    : root_{std::move(root)} {}

std::filesystem::path ApplicationPki::own_certificate() const {
  return root_ / "own" / "certificate.pem";
}

std::filesystem::path ApplicationPki::own_private_key() const {
  return root_ / "own" / "private_key.pem";
}

std::filesystem::path ApplicationPki::trusted_dir() const {
  return root_ / "trusted";
}

std::filesystem::path ApplicationPki::rejected_dir() const {
  return root_ / "rejected";
}

Status ApplicationPki::EnsureOwnCertificate(std::string_view application_uri,
                                            std::string_view host_name) {
  std::error_code ec;
  if (std::filesystem::exists(own_certificate(), ec) &&
      std::filesystem::exists(own_private_key(), ec)) {
    return Status{StatusCode::Good};
  }
  std::filesystem::create_directories(root_ / "own", ec);
  std::filesystem::create_directories(trusted_dir(), ec);
  std::filesystem::create_directories(rejected_dir(), ec);
  if (ec)
    return Status{StatusCode::Bad};

  std::vector<std::string> dns_names;
  if (!host_name.empty())
    dns_names.emplace_back(host_name);
  auto generated = binary::crypto::GenerateApplicationInstanceCertificate(
      application_uri, "Telecontrol SCADA Client", dns_names,
      /*validity_days=*/5 * 365);
  if (!generated.ok())
    return generated.status();
  // The key first and owner-only: a crash between the two writes then leaves
  // a key with no certificate, which the next call regenerates, rather than a
  // certificate whose key was never written.
  if (!WriteFile(own_private_key(), generated->private_key_pem))
    return Status{StatusCode::Bad};
  std::filesystem::permissions(
      own_private_key(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      ec);
  if (!WriteFile(own_certificate(), generated->certificate_pem))
    return Status{StatusCode::Bad};
  return Status{StatusCode::Good};
}

std::optional<ApplicationPki::RejectedCertificate>
ApplicationPki::NewestRejected(std::string_view dialled_host) const {
  std::error_code ec;
  std::optional<std::filesystem::directory_entry> newest;
  for (const auto& entry :
       std::filesystem::directory_iterator{rejected_dir(), ec}) {
    if (!entry.is_regular_file(ec))
      continue;
    if (!newest || entry.last_write_time(ec) > newest->last_write_time(ec))
      newest = entry;
  }
  if (!newest)
    return std::nullopt;

  auto certificate = LoadCertificateFile(newest->path());
  if (!certificate)
    return std::nullopt;
  RejectedCertificate result{
      .file = newest->path(),
      .sha256_fingerprint =
          binary::crypto::CertificateSha256Fingerprint(*certificate),
      .subject = binary::crypto::CertificateSubject(*certificate),
      .application_uris = binary::crypto::CertificateUris(*certificate),
  };

  for (const auto& entry :
       std::filesystem::directory_iterator{trusted_dir(), ec}) {
    auto trusted = LoadCertificateFile(entry.path());
    if (!trusted)
      continue;
    if (!dialled_host.empty() &&
        binary::crypto::CertificateMatchesHost(*trusted, dialled_host) &&
        binary::crypto::CertificateSha256Fingerprint(*trusted) !=
            result.sha256_fingerprint) {
      result.replaces_trusted = true;
      break;
    }
  }
  return result;
}

Status ApplicationPki::Trust(const RejectedCertificate& certificate) {
  std::error_code ec;
  std::filesystem::create_directories(trusted_dir(), ec);
  std::filesystem::rename(certificate.file,
                          trusted_dir() / certificate.file.filename(), ec);
  return ec ? Status{StatusCode::Bad} : Status{StatusCode::Good};
}

}  // namespace opcua
