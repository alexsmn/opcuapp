#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// OpenSSL forward-declarations to keep this header lean.
struct x509_st;
using X509 = x509_st;
struct evp_pkey_st;
using EVP_PKEY = evp_pkey_st;

namespace opcua::binary::crypto {

// RAII wrappers around the OpenSSL handles. Constructed via Load* helpers
// below; callers pass them by (const) reference into the sign/encrypt
// primitives.
class Certificate {
 public:
  explicit Certificate(X509* owned = nullptr) noexcept;
  ~Certificate();
  Certificate(const Certificate&) = delete;
  Certificate& operator=(const Certificate&) = delete;
  Certificate(Certificate&& other) noexcept;
  Certificate& operator=(Certificate&& other) noexcept;

  [[nodiscard]] X509* raw() const { return handle_; }
  [[nodiscard]] bool empty() const { return handle_ == nullptr; }

 private:
  X509* handle_ = nullptr;
};

class PrivateKey {
 public:
  explicit PrivateKey(EVP_PKEY* owned = nullptr) noexcept;
  ~PrivateKey();
  PrivateKey(const PrivateKey&) = delete;
  PrivateKey& operator=(const PrivateKey&) = delete;
  PrivateKey(PrivateKey&& other) noexcept;
  PrivateKey& operator=(PrivateKey&& other) noexcept;

  [[nodiscard]] EVP_PKEY* raw() const { return handle_; }
  [[nodiscard]] bool empty() const { return handle_ == nullptr; }
  [[nodiscard]] int KeySizeBytes() const;  // 256 for a 2048-bit RSA key.

 private:
  EVP_PKEY* handle_ = nullptr;
};

// ---- Cert / key load + descriptors -----------------------------------------

// Parse a PEM-encoded X.509 certificate.
[[nodiscard]] StatusOr<Certificate> LoadPemCertificate(std::string_view pem);

// Parse a DER-encoded X.509 certificate. The OPC UA wire format carries
// certificates (e.g. EndpointDescription.serverCertificate) as DER ByteStrings,
// so this is the loader used for certificates obtained via discovery.
[[nodiscard]] StatusOr<Certificate> LoadDerCertificate(
    std::span<const std::uint8_t> der);

// Generates `length` cryptographically-random bytes from the platform CSPRNG,
// e.g. the client nonce sent in CreateSessionRequest.
[[nodiscard]] StatusOr<ByteString> GenerateNonce(std::size_t length);

// Parse a PEM-encoded PKCS#8 or PKCS#1 RSA private key. Optional
// `passphrase` unlocks encrypted keys; pass an empty string_view when the
// key is not encrypted.
[[nodiscard]] StatusOr<PrivateKey> LoadPemPrivateKey(
    std::string_view pem,
    std::string_view passphrase = {});

// DER encoding of the certificate (for the SenderCertificate field of the
// OPC UA asymmetric security header).
[[nodiscard]] StatusOr<ByteString> CertificateDer(const Certificate& cert);

// SHA-1 thumbprint of the DER-encoded certificate (for the
// ReceiverCertificateThumbprint field).
[[nodiscard]] StatusOr<ByteString> CertificateThumbprint(
    const Certificate& cert);

// True if the certificate's validity period (notBefore..notAfter) contains the
// current time. Used by the trust store to reject expired/not-yet-valid client
// certificates (OPC UA Part 4 §6.1.3).
[[nodiscard]] bool CertificateTimeValid(const Certificate& cert);

// Returns the RSA public key of the certificate's subject, ready to hand
// to RsaOaepEncrypt / RsaPkcs1Sha256Verify below. The returned PrivateKey
// contains only the public parameters.
[[nodiscard]] StatusOr<PrivateKey> CertificatePublicKey(
    const Certificate& cert);

// ---- Asymmetric primitives (Basic256Sha256) --------------------------------

// OPC UA Part 4 §7 asymmetric security:
//   - encryption: RSA-OAEP with SHA-1 / MGF1(SHA-1)
//   - signature:  RSA-PKCS#1-v1_5 with SHA-256
//
// Input plaintext/ciphertext sizes are constrained by the RSA modulus
// (see PrivateKey::KeySizeBytes). RsaOaepEncrypt / Decrypt chunk the
// payload in (KeySizeBytes - 42) / KeySizeBytes blocks respectively.
[[nodiscard]] StatusOr<ByteString> RsaOaepEncrypt(
    const PrivateKey& public_key,
    std::span<const std::uint8_t> plaintext);
[[nodiscard]] StatusOr<ByteString> RsaOaepDecrypt(
    const PrivateKey& private_key,
    std::span<const std::uint8_t> ciphertext);

[[nodiscard]] StatusOr<ByteString> RsaPkcs1Sha256Sign(
    const PrivateKey& private_key,
    std::span<const std::uint8_t> data);
[[nodiscard]] bool RsaPkcs1Sha256Verify(
    const PrivateKey& public_key,
    std::span<const std::uint8_t> data,
    std::span<const std::uint8_t> signature);

// ---- Symmetric primitives (Basic256Sha256) ---------------------------------

// HMAC-SHA256 of `data` keyed by `key`. Always returns a 32-byte tag.
[[nodiscard]] ByteString HmacSha256(std::span<const std::uint8_t> key,
                                    std::span<const std::uint8_t> data);

// AES-256-CBC. The key must be 32 bytes; the IV must be 16 bytes.
// Plaintext must already be padded to a multiple of 16 bytes: OPC UA
// dictates padding inside the secure-message body (the PaddingSize field),
// not PKCS#7, so we do not auto-pad or auto-strip here.
[[nodiscard]] StatusOr<ByteString> AesCbcEncrypt(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> iv,
    std::span<const std::uint8_t> plaintext);
[[nodiscard]] StatusOr<ByteString> AesCbcDecrypt(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> iv,
    std::span<const std::uint8_t> ciphertext);

// P_SHA256 PRF (RFC 5246 §5) used by OPC UA key derivation. Expands
// (secret, seed) into `out_len` bytes.
[[nodiscard]] ByteString PSha256(std::span<const std::uint8_t> secret,
                                 std::span<const std::uint8_t> seed,
                                 std::size_t out_len);

// OPC UA Part 6 §6.7.5 key derivation: given the peer's nonce and our own
// nonce (server and client for derivation of our side, or swapped for the
// peer side), derive 32-byte signing key, 32-byte encrypting key, and
// 16-byte IV.
struct DerivedKeys {
  ByteString signing_key;            // 32 bytes
  ByteString encrypting_key;         // 32 bytes
  ByteString initialization_vector;  // 16 bytes
};
[[nodiscard]] DerivedKeys DeriveBasic256Sha256Keys(
    std::span<const std::uint8_t> secret,
    std::span<const std::uint8_t> seed);

}  // namespace opcua::binary::crypto
