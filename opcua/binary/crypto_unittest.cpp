#include "opcua/binary/crypto.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opcua::binary::crypto {
namespace {

// --- On-the-fly RSA test cert / key -----------------------------------------

// Generate a fresh 2048-bit RSA keypair and wrap it in a self-signed X.509
// cert so the tests can round-trip through LoadPemCertificate +
// LoadPemPrivateKey without needing external fixtures.
struct PemKeypair {
  std::string cert_pem;
  std::string private_key_pem;
};

PemKeypair GenerateSelfSignedRsa() {
  EVP_PKEY* key = nullptr;
  {
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (kctx == nullptr ||
        EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0 ||
        EVP_PKEY_keygen(kctx, &key) <= 0) {
      if (kctx) EVP_PKEY_CTX_free(kctx);
      return {};
    }
    EVP_PKEY_CTX_free(kctx);
  }
  X509* cert = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 30);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-test"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_sign(cert, key, EVP_sha256());

  PemKeypair result;
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    result.cert_pem.assign(data, len);
    BIO_free(bio);
  }
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    result.private_key_pem.assign(data, len);
    BIO_free(bio);
  }
  X509_free(cert);
  EVP_PKEY_free(key);
  return result;
}

std::span<const std::uint8_t> AsByteSpan(const scada::ByteString& bs) {
  return {reinterpret_cast<const std::uint8_t*>(bs.data()), bs.size()};
}

// -----------------------------------------------------------------------------

TEST(CryptoTest, LoadPemCertificateAndKey) {
  const auto keypair = GenerateSelfSignedRsa();
  ASSERT_FALSE(keypair.cert_pem.empty());

  auto cert = LoadPemCertificate(keypair.cert_pem);
  ASSERT_TRUE(cert.ok());
  EXPECT_FALSE(cert->empty());

  auto key = LoadPemPrivateKey(keypair.private_key_pem);
  ASSERT_TRUE(key.ok());
  EXPECT_EQ(key->KeySizeBytes(), 256);  // 2048-bit RSA -> 256 bytes

  auto der = CertificateDer(*cert);
  ASSERT_TRUE(der.ok());
  EXPECT_FALSE(der->empty());
}

TEST(CryptoTest, LoadPemCertificateRejectsGarbage) {
  const auto cert = LoadPemCertificate("not a pem certificate");
  EXPECT_FALSE(cert.ok());
}

TEST(CryptoTest, ThumbprintIs20Bytes) {
  const auto keypair = GenerateSelfSignedRsa();
  auto cert = LoadPemCertificate(keypair.cert_pem);
  ASSERT_TRUE(cert.ok());
  auto thumbprint = CertificateThumbprint(*cert);
  ASSERT_TRUE(thumbprint.ok());
  EXPECT_EQ(thumbprint->size(), 20u);

  // A second call must produce the identical digest.
  auto second = CertificateThumbprint(*cert);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(*thumbprint, *second);
}

TEST(CryptoTest, RsaOaepRoundTripShortMessage) {
  const auto keypair = GenerateSelfSignedRsa();
  auto priv = LoadPemPrivateKey(keypair.private_key_pem);
  ASSERT_TRUE(priv.ok());
  auto cert = LoadPemCertificate(keypair.cert_pem);
  ASSERT_TRUE(cert.ok());
  auto pub = CertificatePublicKey(*cert);
  ASSERT_TRUE(pub.ok());

  const std::array<std::uint8_t, 32> plaintext{
      'h', 'e', 'l', 'l', 'o', 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
      11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
  };
  auto ciphertext = RsaOaepEncrypt(*pub, plaintext);
  ASSERT_TRUE(ciphertext.ok());
  EXPECT_EQ(ciphertext->size(), 256u);  // one block of 2048-bit key

  auto decrypted = RsaOaepDecrypt(*priv, AsByteSpan(*ciphertext));
  ASSERT_TRUE(decrypted.ok());
  ASSERT_EQ(decrypted->size(), plaintext.size());
  EXPECT_EQ(
      std::memcmp(decrypted->data(), plaintext.data(), plaintext.size()), 0);
}

TEST(CryptoTest, RsaOaepMultiBlockRoundTrip) {
  const auto keypair = GenerateSelfSignedRsa();
  auto priv = LoadPemPrivateKey(keypair.private_key_pem);
  ASSERT_TRUE(priv.ok());
  auto cert = LoadPemCertificate(keypair.cert_pem);
  ASSERT_TRUE(cert.ok());
  auto pub = CertificatePublicKey(*cert);
  ASSERT_TRUE(pub.ok());

  // 400 bytes — larger than one OAEP plaintext block (214 bytes for a
  // 2048-bit key), so the encrypt chunks twice into two 256-byte blocks.
  std::vector<std::uint8_t> plaintext(400);
  for (std::size_t i = 0; i < plaintext.size(); ++i) {
    plaintext[i] = static_cast<std::uint8_t>(i * 31);
  }
  auto ciphertext = RsaOaepEncrypt(*pub, plaintext);
  ASSERT_TRUE(ciphertext.ok());
  EXPECT_EQ(ciphertext->size(), 2u * 256u);

  auto decrypted = RsaOaepDecrypt(*priv, AsByteSpan(*ciphertext));
  ASSERT_TRUE(decrypted.ok());
  ASSERT_EQ(decrypted->size(), plaintext.size());
  EXPECT_EQ(
      std::memcmp(decrypted->data(), plaintext.data(), plaintext.size()), 0);
}

TEST(CryptoTest, RsaPkcs1Sha256SignAndVerify) {
  const auto keypair = GenerateSelfSignedRsa();
  auto priv = LoadPemPrivateKey(keypair.private_key_pem);
  ASSERT_TRUE(priv.ok());
  auto cert = LoadPemCertificate(keypair.cert_pem);
  ASSERT_TRUE(cert.ok());
  auto pub = CertificatePublicKey(*cert);
  ASSERT_TRUE(pub.ok());

  const std::string data = "Part6Test";
  std::span<const std::uint8_t> data_span{
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
  auto signature = RsaPkcs1Sha256Sign(*priv, data_span);
  ASSERT_TRUE(signature.ok());
  EXPECT_EQ(signature->size(), 256u);

  EXPECT_TRUE(RsaPkcs1Sha256Verify(*pub, data_span, AsByteSpan(*signature)));

  // Tampered payload must fail verification.
  std::string tampered = data;
  tampered[0] ^= 0x01;
  std::span<const std::uint8_t> tampered_span{
      reinterpret_cast<const std::uint8_t*>(tampered.data()), tampered.size()};
  EXPECT_FALSE(
      RsaPkcs1Sha256Verify(*pub, tampered_span, AsByteSpan(*signature)));
}

TEST(CryptoTest, HmacSha256IsDeterministic) {
  const std::array<std::uint8_t, 32> key{};  // all zeros
  const std::string data = "payload";
  std::span<const std::uint8_t> data_span{
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
  const auto mac_a = HmacSha256(key, data_span);
  const auto mac_b = HmacSha256(key, data_span);
  EXPECT_EQ(mac_a, mac_b);
  EXPECT_EQ(mac_a.size(), 32u);
}

TEST(CryptoTest, HmacSha256KeyDifferenceChangesTag) {
  std::array<std::uint8_t, 32> key_a{};
  std::array<std::uint8_t, 32> key_b{};
  key_b[0] = 1;
  const std::string data = "payload";
  std::span<const std::uint8_t> data_span{
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
  EXPECT_NE(HmacSha256(key_a, data_span), HmacSha256(key_b, data_span));
}

TEST(CryptoTest, AesCbcRoundTrip) {
  std::array<std::uint8_t, 32> key{};
  std::array<std::uint8_t, 16> iv{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::uint8_t>(i);
  }
  for (std::size_t i = 0; i < iv.size(); ++i) {
    iv[i] = static_cast<std::uint8_t>(i * 3);
  }
  std::vector<std::uint8_t> plaintext(64);
  for (std::size_t i = 0; i < plaintext.size(); ++i) {
    plaintext[i] = static_cast<std::uint8_t>(i);
  }

  auto ciphertext = AesCbcEncrypt(key, iv, plaintext);
  ASSERT_TRUE(ciphertext.ok());
  EXPECT_EQ(ciphertext->size(), plaintext.size());
  EXPECT_NE(
      std::memcmp(ciphertext->data(), plaintext.data(), plaintext.size()), 0);

  auto decrypted = AesCbcDecrypt(key, iv, AsByteSpan(*ciphertext));
  ASSERT_TRUE(decrypted.ok());
  EXPECT_EQ(decrypted->size(), plaintext.size());
  EXPECT_EQ(
      std::memcmp(decrypted->data(), plaintext.data(), plaintext.size()), 0);
}

TEST(CryptoTest, AesCbcRejectsNonBlockMultiple) {
  std::array<std::uint8_t, 32> key{};
  std::array<std::uint8_t, 16> iv{};
  std::vector<std::uint8_t> plaintext(15);  // not a multiple of 16
  EXPECT_FALSE(AesCbcEncrypt(key, iv, plaintext).ok());
}

TEST(CryptoTest, PSha256ExpandsToRequestedLength) {
  const std::array<std::uint8_t, 4> secret{1, 2, 3, 4};
  const std::array<std::uint8_t, 4> seed{5, 6, 7, 8};

  const auto out_32 = PSha256(secret, seed, 32);
  EXPECT_EQ(out_32.size(), 32u);
  const auto out_80 = PSha256(secret, seed, 80);
  EXPECT_EQ(out_80.size(), 80u);

  // Deterministic: calling twice yields the same bytes.
  const auto out_again = PSha256(secret, seed, 32);
  EXPECT_EQ(out_32, out_again);

  // A different seed produces a different output.
  const std::array<std::uint8_t, 4> seed2{5, 6, 7, 9};
  EXPECT_NE(PSha256(secret, seed2, 32), out_32);
}

TEST(CryptoTest, DeriveBasic256Sha256KeysShape) {
  const std::array<std::uint8_t, 32> secret{};
  const std::array<std::uint8_t, 32> seed{};
  const auto keys = DeriveBasic256Sha256Keys(secret, seed);
  EXPECT_EQ(keys.signing_key.size(), 32u);
  EXPECT_EQ(keys.encrypting_key.size(), 32u);
  EXPECT_EQ(keys.initialization_vector.size(), 16u);
  // The three segments must be distinct (no accidental overlap / repeats).
  EXPECT_NE(keys.signing_key, keys.encrypting_key);
}

}  // namespace
}  // namespace opcua::binary::crypto
