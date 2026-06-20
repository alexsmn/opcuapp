#include "opcua/server_session_manager.h"

#include "opcua/scada/authentication_adapters.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/binary/crypto.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <span>
#include <string>

namespace opcua {
namespace {

std::span<const std::uint8_t> ByteSpan(const scada::ByteString& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// A self-signed RSA client identity: its certificate (DER) and private key.
struct ClientIdentity {
  scada::ByteString certificate_der;
  binary::crypto::PrivateKey private_key;
};

ClientIdentity GenerateClientIdentity() {
  EVP_PKEY* key = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);

  X509* cert = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 30);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-client"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_sign(cert, key, EVP_sha256());

  std::string cert_pem;
  std::string key_pem;
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    cert_pem.assign(data, len);
    BIO_free(bio);
  }
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    key_pem.assign(data, len);
    BIO_free(bio);
  }
  X509_free(cert);
  EVP_PKEY_free(key);

  auto certificate = binary::crypto::LoadPemCertificate(cert_pem);
  auto private_key = binary::crypto::LoadPemPrivateKey(key_pem);
  auto der = binary::crypto::CertificateDer(*certificate);
  return ClientIdentity{.certificate_der = std::move(*der),
                        .private_key = std::move(*private_key)};
}

// RSA-PKCS#1-SHA256 over (server_certificate || server_nonce).
scada::ByteString SignActivation(const binary::crypto::PrivateKey& key,
                                 const scada::ByteString& server_certificate,
                                 const scada::ByteString& server_nonce) {
  scada::ByteString data;
  data.insert(data.end(), server_certificate.begin(), server_certificate.end());
  data.insert(data.end(), server_nonce.begin(), server_nonce.end());
  return *binary::crypto::RsaPkcs1Sha256Sign(key, ByteSpan(data));
}

class ServerSessionManagerTest : public testing::Test {
 protected:
  base::Time now_ = base::Time::Now();
  TestExecutor executor_;

  ServerSessionManager MakeManager() {
    return ServerSessionManager{{
        .authenticator = scada::MakeCoroutineAuthenticator(
            [](scada::LocalizedText user_name,
               scada::LocalizedText password)
                -> Awaitable<scada::StatusOr<scada::AuthenticationResult>> {
              EXPECT_EQ(user_name, scada::LocalizedText{u"operator"});
              EXPECT_EQ(password, scada::LocalizedText{u"secret"});
              co_return scada::AuthenticationResult{
                  .user_id = scada::NodeId{42, 4}, .multi_sessions = true};
            }),
        .now = [this] { return now_; },
        .min_timeout = base::TimeDelta::FromSeconds(10),
    }};
  }
};

TEST_F(ServerSessionManagerTest, ActivatesDetachesResumesAndClosesSession) {
  auto manager = MakeManager();

  const auto created = WaitAwaitable(executor_, manager.CreateSession({}));
  EXPECT_EQ(created.status.code(), scada::StatusCode::Good);
  EXPECT_FALSE(created.session_id.is_null());
  EXPECT_FALSE(created.authentication_token.is_null());

  const auto activated = WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = created.session_id,
          .authentication_token = created.authentication_token,
          .user_name = scada::LocalizedText{u"operator"},
          .password = scada::LocalizedText{u"secret"},
      }));
  EXPECT_EQ(activated.status.code(), scada::StatusCode::Good);
  EXPECT_FALSE(activated.resumed);
  ASSERT_TRUE(activated.authentication_result.has_value());
  EXPECT_EQ(activated.authentication_result->user_id, scada::NodeId(42, 4));

  manager.DetachSession(created.authentication_token);
  const auto detached = manager.FindSession(created.authentication_token);
  ASSERT_TRUE(detached.has_value());
  EXPECT_FALSE(detached->attached);

  const auto resumed = WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = created.session_id,
          .authentication_token = created.authentication_token,
      }));
  EXPECT_EQ(resumed.status.code(), scada::StatusCode::Good);
  EXPECT_TRUE(resumed.resumed);

  const auto closed = manager.CloseSession(
      {.session_id = created.session_id,
       .authentication_token = created.authentication_token});
  EXPECT_EQ(closed.status.code(), scada::StatusCode::Good);
  EXPECT_FALSE(manager.FindSession(created.authentication_token).has_value());
}

// --- Secured session (Basic256Sha256) tests --------------------------------

class ServerSessionManagerSecureTest : public ServerSessionManagerTest {
 protected:
  const scada::ByteString server_certificate_{'S', 'R', 'V', 'C', 'E', 'R', 'T'};

  ServerSessionManager MakeSecureManager() {
    return ServerSessionManager{{
        .authenticator = scada::MakeCoroutineAuthenticator(
            [](scada::LocalizedText, scada::LocalizedText)
                -> Awaitable<scada::StatusOr<scada::AuthenticationResult>> {
              co_return scada::AuthenticationResult{.user_id =
                                                        scada::NodeId{42, 4}};
            }),
        .server_certificate = server_certificate_,
        .now = [this] { return now_; },
        .min_timeout = base::TimeDelta::FromSeconds(10),
    }};
  }
};

TEST_F(ServerSessionManagerSecureTest, AcceptsValidClientSignature) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();

  const auto created = WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = client.certificate_der,
                 }));
  ASSERT_EQ(created.status.code(), scada::StatusCode::Good);
  EXPECT_EQ(created.server_certificate, server_certificate_);
  EXPECT_EQ(created.server_nonce.size(), 32u);

  const auto signature = SignActivation(client.private_key,
                                        created.server_certificate,
                                        created.server_nonce);
  const auto activated = WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .allow_anonymous = true,
                     .client_signature_algorithm =
                         "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256",
                     .client_signature = signature,
                 }));
  EXPECT_EQ(activated.status.code(), scada::StatusCode::Good);
}

TEST_F(ServerSessionManagerSecureTest, RejectsMissingClientSignature) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();

  const auto created = WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = client.certificate_der,
                 }));
  ASSERT_EQ(created.status.code(), scada::StatusCode::Good);

  const auto activated = WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .allow_anonymous = true,
                 }));
  EXPECT_EQ(activated.status.code(),
            scada::StatusCode::Bad_ApplicationSignatureInvalid);
}

TEST_F(ServerSessionManagerSecureTest, RejectsSignatureOverWrongData) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();

  const auto created = WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = client.certificate_der,
                 }));
  ASSERT_EQ(created.status.code(), scada::StatusCode::Good);

  // Sign over a different nonce than the one bound to the session.
  const auto signature = SignActivation(
      client.private_key, created.server_certificate, scada::ByteString(32, 0));
  const auto activated = WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .allow_anonymous = true,
                     .client_signature = signature,
                 }));
  EXPECT_EQ(activated.status.code(),
            scada::StatusCode::Bad_ApplicationSignatureInvalid);
}

TEST_F(ServerSessionManagerSecureTest, RejectsCreateSessionCertificateMismatch) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();
  const auto other = GenerateClientIdentity();

  // Body certificate differs from the SecureChannel certificate.
  const auto created = WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = other.certificate_der,
                 }));
  EXPECT_EQ(created.status.code(),
            scada::StatusCode::Bad_ApplicationSignatureInvalid);
}

}  // namespace
}  // namespace opcua
