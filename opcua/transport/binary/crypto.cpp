#include "opcua/transport/binary/crypto.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace opcua::binary::crypto {
namespace {

constexpr int kSha1DigestSize = 20;
constexpr int kSha256DigestSize = 32;
constexpr int kRsaOaepSha1Overhead = 42;  // 2 * SHA-1 digest + 2
constexpr int kAesBlockSize = 16;

Status BadCrypto() {
  return Status{StatusCode::Bad};
}

}  // namespace

// ---------------------------------------------------------------------------
// Certificate

Certificate::Certificate(X509* owned) noexcept : handle_{owned} {}

Certificate::~Certificate() {
  if (handle_) {
    X509_free(handle_);
  }
}

Certificate::Certificate(Certificate&& other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)} {}

Certificate& Certificate::operator=(Certificate&& other) noexcept {
  if (this != &other) {
    if (handle_) {
      X509_free(handle_);
    }
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

// ---------------------------------------------------------------------------
// PrivateKey

PrivateKey::PrivateKey(EVP_PKEY* owned) noexcept : handle_{owned} {}

PrivateKey::~PrivateKey() {
  if (handle_) {
    EVP_PKEY_free(handle_);
  }
}

PrivateKey::PrivateKey(PrivateKey&& other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)} {}

PrivateKey& PrivateKey::operator=(PrivateKey&& other) noexcept {
  if (this != &other) {
    if (handle_) {
      EVP_PKEY_free(handle_);
    }
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

int PrivateKey::KeySizeBytes() const {
  return handle_ ? EVP_PKEY_get_size(handle_) : 0;
}

// ---------------------------------------------------------------------------
// Cert / key load

StatusOr<Certificate> LoadPemCertificate(std::string_view pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (!bio) {
    return StatusOr<Certificate>{BadCrypto()};
  }
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!cert) {
    return StatusOr<Certificate>{BadCrypto()};
  }
  return StatusOr<Certificate>{Certificate{cert}};
}

StatusOr<Certificate> LoadDerCertificate(std::span<const std::uint8_t> der) {
  if (der.empty()) {
    return StatusOr<Certificate>{BadCrypto()};
  }
  const unsigned char* cursor = der.data();
  X509* cert = d2i_X509(nullptr, &cursor, static_cast<long>(der.size()));
  if (!cert) {
    return StatusOr<Certificate>{BadCrypto()};
  }
  return StatusOr<Certificate>{Certificate{cert}};
}

StatusOr<ByteString> GenerateNonce(std::size_t length) {
  ByteString nonce(length);
  if (length > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()),
                               static_cast<int>(length)) != 1) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  return StatusOr<ByteString>{std::move(nonce)};
}

StatusOr<PrivateKey> LoadPemPrivateKey(std::string_view pem,
                                       std::string_view passphrase) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (!bio) {
    return StatusOr<PrivateKey>{BadCrypto()};
  }
  // OpenSSL PEM_read_bio_PrivateKey's password parameter is a non-const
  // void* even though the underlying impl treats it as read-only.
  std::string pass_copy{passphrase};
  void* pass =
      pass_copy.empty() ? nullptr : static_cast<void*>(pass_copy.data());
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, pass);
  BIO_free(bio);
  if (!key) {
    return StatusOr<PrivateKey>{BadCrypto()};
  }
  return StatusOr<PrivateKey>{PrivateKey{key}};
}

StatusOr<ByteString> CertificateDer(const Certificate& cert) {
  if (cert.empty()) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  unsigned char* out = nullptr;
  const int len = i2d_X509(cert.raw(), &out);
  if (len <= 0 || out == nullptr) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  ByteString result(out, out + len);
  OPENSSL_free(out);
  return StatusOr<ByteString>{std::move(result)};
}

StatusOr<ByteString> CertificateThumbprint(const Certificate& cert) {
  auto der = CertificateDer(cert);
  if (!der.ok()) {
    return der;
  }
  ByteString thumbprint(kSha1DigestSize);
  SHA1(reinterpret_cast<const unsigned char*>(der->data()), der->size(),
       reinterpret_cast<unsigned char*>(thumbprint.data()));
  return StatusOr<ByteString>{std::move(thumbprint)};
}

bool CertificateMatchesHost(const Certificate& cert, std::string_view host) {
  if (cert.empty() || host.empty()) {
    return false;
  }
  std::string value{host};
  // A bracketed IPv6 literal from a URL is matched without its brackets.
  if (value.size() > 2 && value.front() == '[' && value.back() == ']') {
    value = value.substr(1, value.size() - 2);
  }
  // X509_check_ip_asc returns 1 on a match and -2 when `value` is not an IP
  // literal, in which case it is a host name.
  const int ip = X509_check_ip_asc(cert.raw(), value.c_str(), 0);
  if (ip != -2) {
    return ip == 1;
  }
  return X509_check_host(cert.raw(), value.data(), value.size(),
                         X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
}

std::vector<std::string> CertificateUris(const Certificate& cert) {
  std::vector<std::string> uris;
  if (cert.empty()) {
    return uris;
  }
  auto* names = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(cert.raw(), NID_subject_alt_name, nullptr, nullptr));
  if (!names) {
    return uris;
  }
  for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    if (name->type != GEN_URI) {
      continue;
    }
    const ASN1_IA5STRING* uri = name->d.uniformResourceIdentifier;
    uris.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)),
                      static_cast<std::size_t>(ASN1_STRING_length(uri)));
  }
  GENERAL_NAMES_free(names);
  return uris;
}

std::string CertificateSha256Fingerprint(const Certificate& cert) {
  if (cert.empty()) {
    return {};
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (X509_digest(cert.raw(), EVP_sha256(), digest, &length) != 1) {
    return {};
  }
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned int i = 0; i < length; ++i) {
    if (i != 0)
      result += ':';
    result += kHex[digest[i] >> 4];
    result += kHex[digest[i] & 0x0f];
  }
  return result;
}

std::string CertificateSubject(const Certificate& cert) {
  if (cert.empty()) {
    return {};
  }
  BIO* bio = BIO_new(BIO_s_mem());
  X509_NAME_print_ex(bio, X509_get_subject_name(cert.raw()), 0,
                     XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB);
  char* data = nullptr;
  const auto length = BIO_get_mem_data(bio, &data);
  std::string result{data, static_cast<std::size_t>(length)};
  BIO_free(bio);
  return result;
}

StatusOr<GeneratedCertificate> GenerateApplicationInstanceCertificate(
    std::string_view application_uri,
    std::string_view common_name,
    std::span<const std::string> dns_names,
    int validity_days) {
  using Result = StatusOr<GeneratedCertificate>;
  EVP_PKEY* key = EVP_RSA_gen(2048);
  if (!key) {
    return Result{BadCrypto()};
  }
  X509* cert = X509_new();
  const auto cleanup = [&] {
    X509_free(cert);
    EVP_PKEY_free(key);
  };

  // A random serial: two certificates from one issuer must not share one, and
  // every self-signed one here is its own issuer.
  std::array<unsigned char, 16> serial{};
  if (RAND_bytes(serial.data(), static_cast<int>(serial.size())) != 1) {
    cleanup();
    return Result{BadCrypto()};
  }
  serial[0] &= 0x7f;
  BIGNUM* serial_bn = BN_bin2bn(serial.data(), serial.size(), nullptr);
  BN_to_ASN1_INTEGER(serial_bn, X509_get_serialNumber(cert));
  BN_free(serial_bn);

  X509_set_version(cert, 2);
  X509_gmtime_adj(X509_get_notBefore(cert), -60 * 60);
  X509_gmtime_adj(X509_get_notAfter(cert), 60L * 60 * 24 * validity_days);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  const std::string cn{common_name};
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                             reinterpret_cast<const unsigned char*>(cn.c_str()),
                             -1, -1, 0);
  X509_set_issuer_name(cert, name);

  std::string san = "URI:" + std::string{application_uri};
  for (const auto& dns : dns_names) {
    // A name with a separator in it would add entries of its own.
    if (dns.find_first_of(",:\n") != std::string::npos) {
      cleanup();
      return Result{Status{StatusCode::Bad_InvalidArgument}};
    }
    san += ",DNS:" + dns;
  }
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  const std::pair<int, std::string> extensions[] = {
      {NID_basic_constraints, "critical,CA:TRUE"},
      {NID_key_usage,
       "critical,digitalSignature,nonRepudiation,keyEncipherment,"
       "dataEncipherment,keyCertSign"},
      {NID_ext_key_usage, "clientAuth,serverAuth"},
      {NID_subject_key_identifier, "hash"},
      {NID_subject_alt_name, san},
  };
  for (const auto& [nid, value] : extensions) {
    X509_EXTENSION* ext =
        X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
    if (!ext) {
      cleanup();
      return Result{BadCrypto()};
    }
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
  }
  if (X509_sign(cert, key, EVP_sha256()) == 0) {
    cleanup();
    return Result{BadCrypto()};
  }

  GeneratedCertificate result;
  const auto to_string = [](BIO* bio) {
    char* data = nullptr;
    const auto length = BIO_get_mem_data(bio, &data);
    std::string text{data, static_cast<std::size_t>(length)};
    BIO_free(bio);
    return text;
  };
  BIO* cert_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(cert_bio, cert);
  result.certificate_pem = to_string(cert_bio);
  BIO* key_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(key_bio, key, nullptr, nullptr, 0, nullptr, nullptr);
  result.private_key_pem = to_string(key_bio);
  cleanup();
  return Result{std::move(result)};
}

bool CertificateTimeValid(const Certificate& cert) {
  if (cert.empty()) {
    return false;
  }
  const ASN1_TIME* not_before = X509_get0_notBefore(cert.raw());
  const ASN1_TIME* not_after = X509_get0_notAfter(cert.raw());
  if (!not_before || !not_after) {
    return false;
  }
  // X509_cmp_current_time returns <0 when the time is in the past, >0 in the
  // future. notBefore must be in the past and notAfter in the future.
  return X509_cmp_current_time(not_before) < 0 &&
         X509_cmp_current_time(not_after) > 0;
}

StatusOr<PrivateKey> CertificatePublicKey(const Certificate& cert) {
  if (cert.empty()) {
    return StatusOr<PrivateKey>{BadCrypto()};
  }
  EVP_PKEY* key = X509_get_pubkey(cert.raw());
  if (!key) {
    return StatusOr<PrivateKey>{BadCrypto()};
  }
  return StatusOr<PrivateKey>{PrivateKey{key}};
}

// ---------------------------------------------------------------------------
// RSA-OAEP encrypt / decrypt
//
// Chunking rule for Basic256Sha256: each plaintext block is
// (KeySizeBytes - 42) bytes; each ciphertext block is KeySizeBytes bytes.

StatusOr<ByteString> RsaOaepEncrypt(const PrivateKey& public_key,
                                    std::span<const std::uint8_t> plaintext) {
  if (public_key.empty()) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  const int key_bytes = public_key.KeySizeBytes();
  const int plain_block = key_bytes - kRsaOaepSha1Overhead;
  if (plain_block <= 0) {
    return StatusOr<ByteString>{BadCrypto()};
  }

  ByteString ciphertext;
  for (std::size_t offset = 0; offset < plaintext.size();
       offset += static_cast<std::size_t>(plain_block)) {
    const std::size_t chunk = std::min(static_cast<std::size_t>(plain_block),
                                       plaintext.size() - offset);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(public_key.raw(), nullptr);
    if (!ctx) {
      return StatusOr<ByteString>{BadCrypto()};
    }
    if (EVP_PKEY_encrypt_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
      EVP_PKEY_CTX_free(ctx);
      return StatusOr<ByteString>{BadCrypto()};
    }
    std::size_t out_len = static_cast<std::size_t>(key_bytes);
    const auto previous_size = ciphertext.size();
    ciphertext.resize(previous_size + out_len);
    if (EVP_PKEY_encrypt(
            ctx,
            reinterpret_cast<unsigned char*>(ciphertext.data() + previous_size),
            &out_len, plaintext.data() + offset, chunk) <= 0) {
      EVP_PKEY_CTX_free(ctx);
      return StatusOr<ByteString>{BadCrypto()};
    }
    ciphertext.resize(previous_size + out_len);
    EVP_PKEY_CTX_free(ctx);
  }
  return StatusOr<ByteString>{std::move(ciphertext)};
}

StatusOr<ByteString> RsaOaepDecrypt(const PrivateKey& private_key,
                                    std::span<const std::uint8_t> ciphertext) {
  if (private_key.empty()) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  const int key_bytes = private_key.KeySizeBytes();
  if (key_bytes <= 0 ||
      ciphertext.size() % static_cast<std::size_t>(key_bytes) != 0) {
    return StatusOr<ByteString>{BadCrypto()};
  }

  ByteString plaintext;
  for (std::size_t offset = 0; offset < ciphertext.size();
       offset += static_cast<std::size_t>(key_bytes)) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(private_key.raw(), nullptr);
    if (!ctx) {
      return StatusOr<ByteString>{BadCrypto()};
    }
    if (EVP_PKEY_decrypt_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
      EVP_PKEY_CTX_free(ctx);
      return StatusOr<ByteString>{BadCrypto()};
    }
    std::size_t out_len = static_cast<std::size_t>(key_bytes);
    const auto previous_size = plaintext.size();
    plaintext.resize(previous_size + out_len);
    if (EVP_PKEY_decrypt(
            ctx,
            reinterpret_cast<unsigned char*>(plaintext.data() + previous_size),
            &out_len, ciphertext.data() + offset,
            static_cast<std::size_t>(key_bytes)) <= 0) {
      EVP_PKEY_CTX_free(ctx);
      return StatusOr<ByteString>{BadCrypto()};
    }
    plaintext.resize(previous_size + out_len);
    EVP_PKEY_CTX_free(ctx);
  }
  return StatusOr<ByteString>{std::move(plaintext)};
}

// ---------------------------------------------------------------------------
// RSA-PKCS#1-v1_5 with SHA-256

StatusOr<ByteString> RsaPkcs1Sha256Sign(const PrivateKey& private_key,
                                        std::span<const std::uint8_t> data) {
  if (private_key.empty()) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr,
                         private_key.raw()) <= 0 ||
      EVP_DigestSignUpdate(ctx, data.data(), data.size()) <= 0) {
    EVP_MD_CTX_free(ctx);
    return StatusOr<ByteString>{BadCrypto()};
  }
  std::size_t sig_len = 0;
  if (EVP_DigestSignFinal(ctx, nullptr, &sig_len) <= 0) {
    EVP_MD_CTX_free(ctx);
    return StatusOr<ByteString>{BadCrypto()};
  }
  ByteString signature(sig_len);
  if (EVP_DigestSignFinal(ctx,
                          reinterpret_cast<unsigned char*>(signature.data()),
                          &sig_len) <= 0) {
    EVP_MD_CTX_free(ctx);
    return StatusOr<ByteString>{BadCrypto()};
  }
  signature.resize(sig_len);
  EVP_MD_CTX_free(ctx);
  return StatusOr<ByteString>{std::move(signature)};
}

bool RsaPkcs1Sha256Verify(const PrivateKey& public_key,
                          std::span<const std::uint8_t> data,
                          std::span<const std::uint8_t> signature) {
  if (public_key.empty()) {
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return false;
  }
  bool ok = false;
  if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr,
                           public_key.raw()) > 0 &&
      EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) > 0) {
    const int result =
        EVP_DigestVerifyFinal(ctx, signature.data(), signature.size());
    ok = result == 1;
  }
  EVP_MD_CTX_free(ctx);
  return ok;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256

ByteString HmacSha256(std::span<const std::uint8_t> key,
                      std::span<const std::uint8_t> data) {
  ByteString out(kSha256DigestSize);
  unsigned int out_len = kSha256DigestSize;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(),
       data.size(), reinterpret_cast<unsigned char*>(out.data()), &out_len);
  out.resize(out_len);
  return out;
}

// ---------------------------------------------------------------------------
// AES-CBC (AES-256 for 32-byte keys, AES-128 for 16-byte keys)

namespace {

const EVP_CIPHER* AesCbcCipherFor(std::size_t key_size) {
  switch (key_size) {
    case 32:
      return EVP_aes_256_cbc();
    case 16:
      return EVP_aes_128_cbc();
    default:
      return nullptr;
  }
}

}  // namespace

StatusOr<ByteString> AesCbcEncrypt(std::span<const std::uint8_t> key,
                                   std::span<const std::uint8_t> iv,
                                   std::span<const std::uint8_t> plaintext) {
  const EVP_CIPHER* cipher = AesCbcCipherFor(key.size());
  if (!cipher || iv.size() != kAesBlockSize ||
      plaintext.size() % kAesBlockSize != 0) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  ByteString out(plaintext.size());
  int out_len = 0;
  int total = 0;
  bool ok =
      EVP_EncryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data()) == 1 &&
      EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
      EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()),
                        &out_len, plaintext.data(),
                        static_cast<int>(plaintext.size())) == 1;
  if (ok) {
    total += out_len;
    ok = EVP_EncryptFinal_ex(
             ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
             &out_len) == 1;
    total += out_len;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  out.resize(total);
  return StatusOr<ByteString>{std::move(out)};
}

StatusOr<ByteString> AesCbcDecrypt(std::span<const std::uint8_t> key,
                                   std::span<const std::uint8_t> iv,
                                   std::span<const std::uint8_t> ciphertext) {
  const EVP_CIPHER* cipher = AesCbcCipherFor(key.size());
  if (!cipher || iv.size() != kAesBlockSize ||
      ciphertext.size() % kAesBlockSize != 0) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  ByteString out(ciphertext.size());
  int out_len = 0;
  int total = 0;
  bool ok =
      EVP_DecryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data()) == 1 &&
      EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
      EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()),
                        &out_len, ciphertext.data(),
                        static_cast<int>(ciphertext.size())) == 1;
  if (ok) {
    total += out_len;
    ok = EVP_DecryptFinal_ex(
             ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
             &out_len) == 1;
    total += out_len;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    return StatusOr<ByteString>{BadCrypto()};
  }
  out.resize(total);
  return StatusOr<ByteString>{std::move(out)};
}

// ---------------------------------------------------------------------------
// P_SHA256 PRF (RFC 5246 §5)

ByteString PSha256(std::span<const std::uint8_t> secret,
                   std::span<const std::uint8_t> seed,
                   std::size_t out_len) {
  ByteString out;
  out.reserve(out_len);

  // A(0) = seed
  ByteString a(seed.begin(), seed.end());

  while (out.size() < out_len) {
    // A(i) = HMAC_SHA256(secret, A(i-1))
    a = HmacSha256(secret,
                   {reinterpret_cast<const std::uint8_t*>(a.data()), a.size()});
    // block = HMAC_SHA256(secret, A(i) || seed)
    ByteString buf;
    buf.reserve(a.size() + seed.size());
    buf.insert(buf.end(), a.begin(), a.end());
    buf.insert(buf.end(), seed.begin(), seed.end());
    const auto block = HmacSha256(
        secret,
        {reinterpret_cast<const std::uint8_t*>(buf.data()), buf.size()});
    const auto take = std::min(block.size(), out_len - out.size());
    out.insert(out.end(), block.begin(), block.begin() + take);
  }
  out.resize(out_len);
  return out;
}

DerivedKeys DeriveBasic256Sha256Keys(std::span<const std::uint8_t> secret,
                                     std::span<const std::uint8_t> seed) {
  return DeriveKeys(secret, seed, 32);
}

DerivedKeys DeriveKeys(std::span<const std::uint8_t> secret,
                       std::span<const std::uint8_t> seed,
                       std::size_t encrypting_key_length) {
  constexpr std::size_t kSigningKeyLen = 32;
  const std::size_t kEncryptingKeyLen = encrypting_key_length;
  constexpr std::size_t kIvLen = 16;
  auto material =
      PSha256(secret, seed, kSigningKeyLen + kEncryptingKeyLen + kIvLen);
  DerivedKeys keys;
  keys.signing_key.assign(material.begin(), material.begin() + kSigningKeyLen);
  keys.encrypting_key.assign(
      material.begin() + kSigningKeyLen,
      material.begin() + kSigningKeyLen + kEncryptingKeyLen);
  keys.initialization_vector.assign(
      material.begin() + kSigningKeyLen + kEncryptingKeyLen,
      material.begin() + kSigningKeyLen + kEncryptingKeyLen + kIvLen);
  return keys;
}

}  // namespace opcua::binary::crypto
