#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/test/test_certificate.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace opcua::binary::crypto {
namespace {

// --- On-the-fly RSA test cert / key -----------------------------------------

// Generate a fresh 2048-bit RSA keypair and wrap it in a self-signed X.509
// cert so the tests can round-trip through LoadPemCertificate +
// LoadPemPrivateKey without needing external fixtures.
using binary::test::GenerateSelfSignedRsa;
using binary::test::PemKeypair;

std::span<const std::uint8_t> AsByteSpan(const opcua::ByteString& bs) {
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
      'h', 'e', 'l', 'l', 'o', 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
      11,  12,  13,  14,  15,  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
  };
  auto ciphertext = RsaOaepEncrypt(*pub, plaintext);
  ASSERT_TRUE(ciphertext.ok());
  EXPECT_EQ(ciphertext->size(), 256u);  // one block of 2048-bit key

  auto decrypted = RsaOaepDecrypt(*priv, AsByteSpan(*ciphertext));
  ASSERT_TRUE(decrypted.ok());
  ASSERT_EQ(decrypted->size(), plaintext.size());
  EXPECT_EQ(std::memcmp(decrypted->data(), plaintext.data(), plaintext.size()),
            0);
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
  EXPECT_EQ(std::memcmp(decrypted->data(), plaintext.data(), plaintext.size()),
            0);
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
  EXPECT_NE(std::memcmp(ciphertext->data(), plaintext.data(), plaintext.size()),
            0);

  auto decrypted = AesCbcDecrypt(key, iv, AsByteSpan(*ciphertext));
  ASSERT_TRUE(decrypted.ok());
  EXPECT_EQ(decrypted->size(), plaintext.size());
  EXPECT_EQ(std::memcmp(decrypted->data(), plaintext.data(), plaintext.size()),
            0);
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

// Aes128_Sha256_RsaOaep derives a 16-byte encrypting key, and the IV follows
// it at offset DerivedSignatureKeyLength + EncryptionKeyLength (OPC UA Part 6
// §6.7.6 Table 66,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7.6), so the IV is
// a different slice of the same P_SHA256 stream than under Basic256Sha256.
TEST(CryptoTest, DeriveKeysForAes128ShiftsTheIv) {
  const std::array<std::uint8_t, 32> secret{1, 2, 3};
  const std::array<std::uint8_t, 32> seed{4, 5, 6};
  const auto aes128 = DeriveKeys(secret, seed, 16);
  const auto aes256 = DeriveKeys(secret, seed, 32);
  EXPECT_EQ(aes128.signing_key, aes256.signing_key);
  ASSERT_EQ(aes128.encrypting_key.size(), 16u);
  EXPECT_TRUE(std::equal(aes128.encrypting_key.begin(),
                         aes128.encrypting_key.end(),
                         aes256.encrypting_key.begin()));
  const auto stream = PSha256(secret, seed, 32 + 16 + 16);
  EXPECT_EQ(aes128.initialization_vector,
            opcua::ByteString(stream.begin() + 48, stream.begin() + 64));
}

// NIST SP 800-38A F.2.1 CBC-AES128.Encrypt, first block; the expected
// ciphertext was confirmed with `openssl enc -aes-128-cbc` (OpenSSL 3.6.3,
// 2026-10-24).
TEST(CryptoTest, AesCbcUsesAes128ForA16ByteKey) {
  const std::array<std::uint8_t, 16> key{0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae,
                                         0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88,
                                         0x09, 0xcf, 0x4f, 0x3c};
  const std::array<std::uint8_t, 16> iv{0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                        0x0c, 0x0d, 0x0e, 0x0f};
  const std::array<std::uint8_t, 16> plaintext{
      0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
      0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
  const std::array<std::uint8_t, 16> expected{
      0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
      0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};
  auto ciphertext = AesCbcEncrypt(key, iv, plaintext);
  ASSERT_TRUE(ciphertext.ok());
  ASSERT_EQ(ciphertext->size(), 16u);
  EXPECT_TRUE(
      std::equal(expected.begin(), expected.end(),
                 reinterpret_cast<const std::uint8_t*>(ciphertext->data())));
  auto decrypted = AesCbcDecrypt(key, iv, AsByteSpan(*ciphertext));
  ASSERT_TRUE(decrypted.ok());
  EXPECT_TRUE(
      std::equal(plaintext.begin(), plaintext.end(),
                 reinterpret_cast<const std::uint8_t*>(decrypted->data())));
}

TEST(CryptoTest, AesCbcRejectsOtherKeySizes) {
  const std::array<std::uint8_t, 24> key{};
  const std::array<std::uint8_t, 16> iv{};
  const std::array<std::uint8_t, 16> plaintext{};
  EXPECT_FALSE(AesCbcEncrypt(key, iv, plaintext).ok());
}

}  // namespace
}  // namespace opcua::binary::crypto
