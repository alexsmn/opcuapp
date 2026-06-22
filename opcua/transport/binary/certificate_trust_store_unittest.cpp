#include "opcua/transport/binary/certificate_trust_store.h"

#include "opcua/types/basic_types.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <vector>

namespace opcua::binary {
namespace {

std::span<const std::uint8_t> ByteSpan(const opcua::ByteString& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// Generates a self-signed RSA certificate and returns its DER encoding. When
// `valid` is false the certificate is already expired.
opcua::ByteString GenerateCertDer(bool valid = true) {
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
  opcua::ByteString der(out, out + len);
  OPENSSL_free(out);
  X509_free(cert);
  EVP_PKEY_free(key);
  return der;
}

void WriteFile(const std::filesystem::path& path,
               const opcua::ByteString& der) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(der.data(), static_cast<std::streamsize>(der.size()));
}

EVP_PKEY* GenerateKey() {
  EVP_PKEY* key = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);
  return key;
}

opcua::ByteString CertToDer(X509* cert) {
  unsigned char* out = nullptr;
  const int len = i2d_X509(cert, &out);
  opcua::ByteString der(out, out + len);
  OPENSSL_free(out);
  return der;
}

opcua::ByteString CrlToDer(X509_CRL* crl) {
  unsigned char* out = nullptr;
  const int len = i2d_X509_CRL(crl, &out);
  opcua::ByteString der(out, out + len);
  OPENSSL_free(out);
  return der;
}

// A CA, a client certificate signed by it, and a CRL (signed by the CA)
// revoking that client certificate — all as DER.
struct ChainFixture {
  opcua::ByteString ca_cert_der;
  opcua::ByteString client_cert_der;
  opcua::ByteString crl_der;
};

ChainFixture MakeChainFixture() {
  EVP_PKEY* ca_key = GenerateKey();
  X509* ca = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(ca), 1);
  X509_gmtime_adj(X509_get_notBefore(ca), 0);
  X509_gmtime_adj(X509_get_notAfter(ca), 60 * 60 * 24 * 30);
  X509_set_pubkey(ca, ca_key);
  X509_NAME* ca_name = X509_get_subject_name(ca);
  X509_NAME_add_entry_by_txt(
      ca_name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-test-ca"), -1, -1, 0);
  X509_set_issuer_name(ca, ca_name);
  if (X509_EXTENSION* ext = X509V3_EXT_conf_nid(
          nullptr, nullptr, NID_basic_constraints, "critical,CA:TRUE")) {
    X509_add_ext(ca, ext, -1);
    X509_EXTENSION_free(ext);
  }
  X509_sign(ca, ca_key, EVP_sha256());

  EVP_PKEY* client_key = GenerateKey();
  X509* client = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(client), 2);
  X509_gmtime_adj(X509_get_notBefore(client), 0);
  X509_gmtime_adj(X509_get_notAfter(client), 60 * 60 * 24 * 30);
  X509_set_pubkey(client, client_key);
  X509_NAME* client_name = X509_get_subject_name(client);
  X509_NAME_add_entry_by_txt(
      client_name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-test-client"), -1, -1, 0);
  X509_set_issuer_name(client, ca_name);
  X509_sign(client, ca_key, EVP_sha256());

  X509_CRL* crl = X509_CRL_new();
  X509_CRL_set_issuer_name(crl, ca_name);
  ASN1_TIME* now = ASN1_TIME_set(nullptr, std::time(nullptr));
  ASN1_TIME* next = ASN1_TIME_adj(nullptr, std::time(nullptr), 30, 0);
  X509_CRL_set1_lastUpdate(crl, now);
  X509_CRL_set1_nextUpdate(crl, next);
  X509_REVOKED* revoked = X509_REVOKED_new();
  X509_REVOKED_set_serialNumber(revoked, X509_get_serialNumber(client));
  X509_REVOKED_set_revocationDate(revoked, now);
  X509_CRL_add0_revoked(crl, revoked);
  X509_CRL_sort(crl);
  X509_CRL_sign(crl, ca_key, EVP_sha256());

  ChainFixture fixture{.ca_cert_der = CertToDer(ca),
                       .client_cert_der = CertToDer(client),
                       .crl_der = CrlToDer(crl)};

  ASN1_TIME_free(now);
  ASN1_TIME_free(next);
  X509_CRL_free(crl);
  X509_free(client);
  EVP_PKEY_free(client_key);
  X509_free(ca);
  EVP_PKEY_free(ca_key);
  return fixture;
}

class CertificateTrustStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = std::filesystem::temp_directory_path() /
            ("opcua_trust_store_test_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    trusted_dir_ = base_ / "trusted";
    issuer_dir_ = base_ / "issuer";
    crl_dir_ = base_ / "crl";
    rejected_dir_ = base_ / "rejected";
    std::filesystem::create_directories(trusted_dir_);
    std::filesystem::create_directories(issuer_dir_);
    std::filesystem::create_directories(crl_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(base_, ec);
  }

  std::filesystem::path base_;
  std::filesystem::path trusted_dir_;
  std::filesystem::path issuer_dir_;
  std::filesystem::path crl_dir_;
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

TEST_F(CertificateTrustStoreTest, AcceptsCertificateChainingToTrustedIssuer) {
  const auto chain = MakeChainFixture();
  WriteFile(issuer_dir_ / "ca.der", chain.ca_cert_der);

  // The client cert is not in the trusted leaf dir, but chains to the CA.
  CertificateTrustStore store{{.trusted_dir = trusted_dir_,
                               .issuer_dir = issuer_dir_,
                               .rejected_dir = rejected_dir_}};
  EXPECT_TRUE(store.Validate(ByteSpan(chain.client_cert_der)).good());

  // An unrelated self-signed cert neither chains nor is explicitly trusted.
  const auto unrelated = GenerateCertDer();
  EXPECT_TRUE(store.Validate(ByteSpan(unrelated)).bad());
}

TEST_F(CertificateTrustStoreTest, RejectsCertificateRevokedByCrl) {
  const auto chain = MakeChainFixture();
  WriteFile(issuer_dir_ / "ca.der", chain.ca_cert_der);
  WriteFile(crl_dir_ / "ca.crl", chain.crl_der);

  CertificateTrustStore store{{.trusted_dir = trusted_dir_,
                               .issuer_dir = issuer_dir_,
                               .crl_dir = crl_dir_,
                               .rejected_dir = rejected_dir_}};
  // The client cert chains to the CA but is revoked by the CRL.
  EXPECT_TRUE(store.Validate(ByteSpan(chain.client_cert_der)).bad());
}

}  // namespace
}  // namespace opcua::binary
