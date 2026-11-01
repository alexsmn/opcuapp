#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/test/test_certificate.h"

#include "opcua/base/test/scoped_temp_dir.h"
#include "opcua/transport/binary/certificate_trust_store.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
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

std::span<const std::uint8_t> AsBytes(const ByteString& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// OPC UA Part 12 PullManagement: an application sends a CSR for a key it made
// itself, the CertificateManager issues a certificate from its CA, and every
// peer trusts the CA rather than each certificate (Part 6 §6.2.4).
TEST(CryptoCertificateManagementTest, IssuedCertificateChainsToTheCa) {
  const auto ca = GenerateCertificateAuthority("Site CA", 3650);
  ASSERT_TRUE(ca.ok());
  const auto ca_cert = LoadPemCertificate(ca->certificate_pem);
  const auto ca_key = LoadPemPrivateKey(ca->private_key_pem);
  ASSERT_TRUE(ca_cert.ok());
  ASSERT_TRUE(ca_key.ok());

  const std::vector<std::string> dns{"EDGE-1", "localhost"};
  const std::vector<std::string> ips{"10.0.0.11"};
  const auto request = GenerateSigningRequest("urn:EDGE-1:scada:iec104",
                                              "EDGE-1 iec104", dns, ips);
  ASSERT_TRUE(request.ok());
  EXPECT_EQ(SigningRequestApplicationUri(request->request_der),
            "urn:EDGE-1:scada:iec104");

  const auto issued =
      IssueCertificate(*ca_cert, *ca_key, request->request_der, 1826);
  ASSERT_TRUE(issued.ok());
  const auto cert = LoadDerCertificate(AsBytes(*issued));
  ASSERT_TRUE(cert.ok());
  EXPECT_EQ(CertificateUris(*cert),
            std::vector<std::string>{"urn:EDGE-1:scada:iec104"});
  EXPECT_TRUE(CertificateMatchesHost(*cert, "EDGE-1"));
  EXPECT_TRUE(CertificateMatchesHost(*cert, "10.0.0.11"));
  // The requester's own key goes with the issued certificate.
  const auto key = LoadPemPrivateKey(request->private_key_pem);
  ASSERT_TRUE(key.ok());
  const std::vector<std::uint8_t> data{1, 2, 3};
  const auto signature = RsaPkcs1Sha256Sign(*key, data);
  ASSERT_TRUE(signature.ok());
  const auto public_key = CertificatePublicKey(*cert);
  ASSERT_TRUE(public_key.ok());
  EXPECT_TRUE(RsaPkcs1Sha256Verify(*public_key, data, AsBytes(*signature)));

  // A trust store holding only the CA accepts it.
  opcua::test::ScopedTempDir dir{"opcua_issued_cert"};
  std::filesystem::create_directories(dir.path() / "issuer");
  std::ofstream{dir.path() / "issuer" / "ca.pem"} << ca->certificate_pem;
  CertificateTrustStore store{
      {.trusted_dir = dir.path() / "trusted", .issuer_dir = dir.path() / "issuer"}};
  EXPECT_TRUE(store.Validate(AsBytes(*issued)).good());

  // A certificate from another CA is not trusted.
  const auto other_ca = GenerateCertificateAuthority("Other CA", 3650);
  ASSERT_TRUE(other_ca.ok());
  const auto other_issued =
      IssueCertificate(*LoadPemCertificate(other_ca->certificate_pem),
                       *LoadPemPrivateKey(other_ca->private_key_pem),
                       request->request_der, 1826);
  ASSERT_TRUE(other_issued.ok());
  EXPECT_FALSE(store.Validate(AsBytes(*other_issued)).good());
}

// The CSR's signature proves its sender holds the key; a request altered on
// the way must not get a certificate.
TEST(CryptoCertificateManagementTest, AlteredRequestIsRefused) {
  const auto ca = GenerateCertificateAuthority("Site CA", 3650);
  ASSERT_TRUE(ca.ok());
  const auto request = GenerateSigningRequest("urn:h:scada:modbus", "h", {}, {});
  ASSERT_TRUE(request.ok());
  ByteString altered = request->request_der;
  altered[altered.size() / 2] ^= 0x01;
  const auto issued =
      IssueCertificate(*LoadPemCertificate(ca->certificate_pem),
                       *LoadPemPrivateKey(ca->private_key_pem), altered, 1826);
  EXPECT_FALSE(issued.ok());
}

}  // namespace
}  // namespace opcua::binary::crypto
