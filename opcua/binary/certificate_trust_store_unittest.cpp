#include "opcua/binary/certificate_trust_store.h"

#include "opcua/scada/basic_types.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <vector>

namespace opcua::binary {
namespace {

std::span<const std::uint8_t> ByteSpan(const scada::ByteString& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// Generates a self-signed RSA certificate and returns its DER encoding. When
// `valid` is false the certificate is already expired.
scada::ByteString GenerateCertDer(bool valid = true) {
  EVP_PKEY* key = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);

  X509* cert = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  if (valid) {
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 30);
  } else {
    X509_gmtime_adj(X509_get_notBefore(cert), -60 * 60 * 24 * 30);
    X509_gmtime_adj(X509_get_notAfter(cert), -60 * 60 * 24);
  }
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-test"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_sign(cert, key, EVP_sha256());

  unsigned char* out = nullptr;
  const int len = i2d_X509(cert, &out);
  scada::ByteString der(out, out + len);
  OPENSSL_free(out);
  X509_free(cert);
  EVP_PKEY_free(key);
  return der;
}

void WriteFile(const std::filesystem::path& path, const scada::ByteString& der) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(der.data(), static_cast<std::streamsize>(der.size()));
}

class CertificateTrustStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = std::filesystem::temp_directory_path() /
            ("opcua_trust_store_test_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    trusted_dir_ = base_ / "trusted";
    rejected_dir_ = base_ / "rejected";
    std::filesystem::create_directories(trusted_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(base_, ec);
  }

  std::filesystem::path base_;
  std::filesystem::path trusted_dir_;
  std::filesystem::path rejected_dir_;
};

TEST_F(CertificateTrustStoreTest, AcceptsTrustedCertificate) {
  const auto trusted = GenerateCertDer();
  WriteFile(trusted_dir_ / "trusted.der", trusted);

  CertificateTrustStore store{
      {.trusted_dir = trusted_dir_, .rejected_dir = rejected_dir_}};
  EXPECT_EQ(store.trusted_count(), 1u);
  EXPECT_TRUE(store.Validate(ByteSpan(trusted)).good());
}

TEST_F(CertificateTrustStoreTest, RejectsUntrustedCertificateAndCopiesIt) {
  const auto trusted = GenerateCertDer();
  WriteFile(trusted_dir_ / "trusted.der", trusted);
  const auto untrusted = GenerateCertDer();

  CertificateTrustStore store{
      {.trusted_dir = trusted_dir_, .rejected_dir = rejected_dir_}};
  EXPECT_TRUE(store.Validate(ByteSpan(untrusted)).bad());

  // The rejected certificate was copied to the rejected directory.
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::is_directory(rejected_dir_, ec));
  EXPECT_FALSE(std::filesystem::is_empty(rejected_dir_, ec));
}

TEST_F(CertificateTrustStoreTest, RejectsExpiredCertificate) {
  const auto expired = GenerateCertDer(/*valid=*/false);
  WriteFile(trusted_dir_ / "expired.der", expired);

  CertificateTrustStore store{
      {.trusted_dir = trusted_dir_, .rejected_dir = rejected_dir_}};
  // Even though its thumbprint is in the trusted dir, it is outside its
  // validity period.
  EXPECT_TRUE(store.Validate(ByteSpan(expired)).bad());
}

}  // namespace
}  // namespace opcua::binary
