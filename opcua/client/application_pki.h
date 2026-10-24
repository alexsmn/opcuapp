#pragma once

#include "opcua/types/status.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opcua {

// One OPC UA client application's own PKI directory, laid out the way
// CertificateTrustStore reads it:
//
//   <root>/own/certificate.pem, <root>/own/private_key.pem
//       this application's instance certificate, generated on first use;
//   <root>/trusted/   server certificates the user decided to trust;
//   <root>/rejected/  server certificates a connect refused, kept so the user
//                     can be asked about them and, if they say so, trust them.
//
// It exists for a client run by a person -- the Qt client -- where trust is
// granted interactively the first time a server is met, rather than
// provisioned by an operator as the tiers' trust directories are. OPC UA Part 4
// §6.1.3 Determining if a Certificate is trusted
// (https://reference.opcfoundation.org/Core/Part4/v105/docs/6.1.3) leaves that
// decision to the application; Part 12 §7.8.2 TrustLists
// (https://reference.opcfoundation.org/Core/Part12/v105/docs/7.8.2) describes
// the same trusted / rejected split for servers managed remotely.
class ApplicationPki {
 public:
  explicit ApplicationPki(std::filesystem::path root);

  [[nodiscard]] std::filesystem::path own_certificate() const;
  [[nodiscard]] std::filesystem::path own_private_key() const;
  [[nodiscard]] std::filesystem::path trusted_dir() const;
  [[nodiscard]] std::filesystem::path rejected_dir() const;

  // Generates this application's instance certificate unless one exists.
  // `application_uri` goes in its subjectAltName (Part 6 §6.2.2), with
  // `host_name` as a DNS entry when non-empty.
  [[nodiscard]] Status EnsureOwnCertificate(std::string_view application_uri,
                                            std::string_view host_name);

  // A server certificate a connect refused, described for the person who is
  // about to decide whether to trust it.
  struct RejectedCertificate {
    std::filesystem::path file;
    std::string sha256_fingerprint;
    std::string subject;
    std::vector<std::string> application_uris;
    // A different certificate already trusted is valid for the host the client
    // dialled: the server at this address changed its identity under the
    // user, which is what a man in the middle looks like (and also what a
    // legitimate rotation looks like, which only the user can tell apart).
    // Keyed on the host, not the applicationUri: every server of one product
    // can share an applicationUri, so a match there says nothing.
    bool replaces_trusted = false;
  };

  // The most recently rejected certificate, if any, judged against the host
  // the refused connect dialled.
  [[nodiscard]] std::optional<RejectedCertificate> NewestRejected(
      std::string_view dialled_host) const;

  // Moves `certificate` from rejected/ into trusted/.
  [[nodiscard]] Status Trust(const RejectedCertificate& certificate);

 private:
  std::filesystem::path root_;
};

}  // namespace opcua
