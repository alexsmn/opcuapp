#include "opcua/client/application_pki.h"

#include "opcua/base/test/scoped_temp_dir.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/test/test_certificate.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace opcua {
namespace {

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream{path, std::ios::binary} << text;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

class ApplicationPkiTest : public ::testing::Test {
 protected:
  test::ScopedTempDir dir_{"opcua_application_pki"};
  ApplicationPki pki_{dir_.path() / "pki"};
};

TEST_F(ApplicationPkiTest, GeneratesAConformantOwnCertificateOnce) {
  ASSERT_TRUE(pki_.EnsureOwnCertificate("urn:host:client", "host").good());
  const auto pem = ReadFile(pki_.own_certificate());
  auto certificate = binary::crypto::LoadPemCertificate(pem);
  ASSERT_TRUE(certificate.ok());
  EXPECT_EQ(binary::crypto::CertificateUris(*certificate),
            std::vector<std::string>{"urn:host:client"});
  EXPECT_TRUE(binary::crypto::CertificateMatchesHost(*certificate, "host"));
  EXPECT_TRUE(binary::crypto::CertificateTimeValid(*certificate));
  auto key =
      binary::crypto::LoadPemPrivateKey(ReadFile(pki_.own_private_key()));
  ASSERT_TRUE(key.ok());

  // A second call keeps the identity rather than minting a new one.
  ASSERT_TRUE(pki_.EnsureOwnCertificate("urn:host:client", "host").good());
  EXPECT_EQ(ReadFile(pki_.own_certificate()), pem);
}

TEST_F(ApplicationPkiTest, NothingRejectedMeansNothingToAsk) {
  EXPECT_FALSE(pki_.NewestRejected("demo.example").has_value());
}

TEST_F(ApplicationPkiTest, DescribesAndTrustsTheRejectedCertificate) {
  const auto server =
      binary::test::GenerateSelfSignedRsa({.common_name = "demo",
                                           .uris = {"urn:demo"},
                                           .dns_names = {"demo.example"}});
  WriteFile(pki_.rejected_dir() / "abc.pem", server.cert_pem);

  const auto rejected = pki_.NewestRejected("demo.example");
  ASSERT_TRUE(rejected.has_value());
  auto certificate = binary::crypto::LoadPemCertificate(server.cert_pem);
  EXPECT_EQ(rejected->sha256_fingerprint,
            binary::crypto::CertificateSha256Fingerprint(*certificate));
  EXPECT_EQ(rejected->sha256_fingerprint.size(), 32u * 3 - 1);
  EXPECT_NE(rejected->subject.find("demo"), std::string::npos);
  EXPECT_FALSE(rejected->replaces_trusted);

  ASSERT_TRUE(pki_.Trust(*rejected).good());
  EXPECT_TRUE(std::filesystem::exists(pki_.trusted_dir() / "abc.pem"));
  EXPECT_FALSE(pki_.NewestRejected("demo.example").has_value());
}

TEST_F(ApplicationPkiTest, ADifferentCertificateForATrustedHostIsFlagged) {
  const auto old_server = binary::test::GenerateSelfSignedRsa(
      {.uris = {"urn:demo"}, .dns_names = {"demo.example"}});
  const auto new_server = binary::test::GenerateSelfSignedRsa(
      {.uris = {"urn:demo"}, .dns_names = {"demo.example"}});
  WriteFile(pki_.trusted_dir() / "old.pem", old_server.cert_pem);
  WriteFile(pki_.rejected_dir() / "new.pem", new_server.cert_pem);

  ASSERT_TRUE(pki_.NewestRejected("demo.example").has_value());
  EXPECT_TRUE(pki_.NewestRejected("demo.example")->replaces_trusted);
  // Another host sharing the applicationUri is a first meeting, not a change.
  EXPECT_FALSE(pki_.NewestRejected("other.example")->replaces_trusted);
}

}  // namespace
}  // namespace opcua
