#pragma once

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <string>
#include <vector>

namespace opcua::binary::test {

// A test application instance certificate and its private key, as PEM.
struct PemKeypair {
  std::string cert_pem;
  std::string private_key_pem;
};

// What to put in a generated certificate. The defaults make the plain
// self-signed certificate the secure-channel tests have always used: no
// subjectAltName, valid for 30 days from now.
struct TestCertificateOptions {
  std::string common_name = "opc-ua-test";
  // subjectAltName entries: URIs (an application instance certificate carries
  // its applicationUri there, OPC UA Part 6 §6.2.2,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/6.2.2), DNS names
  // and IP literals.
  std::vector<std::string> uris;
  std::vector<std::string> dns_names;
  std::vector<std::string> ip_addresses;
  long valid_from_seconds = 0;
  long valid_to_seconds = 60L * 60 * 24 * 30;
};

// Generates a fresh 2048-bit RSA keypair in a self-signed X.509 certificate.
inline PemKeypair GenerateSelfSignedRsa(
    const TestCertificateOptions& options = {}) {
  EVP_PKEY* key = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);

  X509* cert = X509_new();
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), options.valid_from_seconds);
  X509_gmtime_adj(X509_get_notAfter(cert), options.valid_to_seconds);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>(options.common_name.c_str()), -1,
      -1, 0);
  X509_set_issuer_name(cert, name);

  std::string san;
  const auto append = [&san](const char* kind, const std::string& value) {
    if (!san.empty())
      san += ',';
    san += kind;
    san += ':';
    san += value;
  };
  for (const auto& uri : options.uris)
    append("URI", uri);
  for (const auto& dns : options.dns_names)
    append("DNS", dns);
  for (const auto& ip : options.ip_addresses)
    append("IP", ip);
  if (!san.empty()) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext =
        X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san.c_str());
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
  }
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

}  // namespace opcua::binary::test
