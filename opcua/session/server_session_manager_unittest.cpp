#include "opcua/session/server_session_manager.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/transport/binary/crypto.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>

namespace opcua {
namespace {

std::span<const std::uint8_t> ByteSpan(const opcua::ByteString& v) {
  return {reinterpret_cast<const std::uint8_t*>(v.data()), v.size()};
}

// A self-signed RSA client identity: its certificate (DER) and private key.
struct ClientIdentity {
  opcua::ByteString certificate_der;
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
opcua::ByteString SignActivation(const binary::crypto::PrivateKey& key,
                                 const opcua::ByteString& server_certificate,
                                 const opcua::ByteString& server_nonce) {
  opcua::ByteString data;
  data.insert(data.end(), server_certificate.begin(), server_certificate.end());
  data.insert(data.end(), server_nonce.begin(), server_nonce.end());
  return *binary::crypto::RsaPkcs1Sha256Sign(key, ByteSpan(data));
}

// Encrypts a UserNameIdentityToken password: RSA-OAEP of
// [length(UInt32 LE) || password || server_nonce] under the server public key.
opcua::ByteString EncryptUserToken(
    const binary::crypto::PrivateKey& server_public_key,
    const std::string& password,
    const opcua::ByteString& server_nonce) {
  const std::uint32_t length =
      static_cast<std::uint32_t>(password.size() + server_nonce.size());
  opcua::ByteString plain;
  plain.push_back(static_cast<char>(length & 0xff));
  plain.push_back(static_cast<char>((length >> 8) & 0xff));
  plain.push_back(static_cast<char>((length >> 16) & 0xff));
  plain.push_back(static_cast<char>((length >> 24) & 0xff));
  plain.insert(plain.end(), password.begin(), password.end());
  plain.insert(plain.end(), server_nonce.begin(), server_nonce.end());
  return *binary::crypto::RsaOaepEncrypt(server_public_key, ByteSpan(plain));
}

// Builds a session manager whose user-token decryptor uses `server_key`.
ServerSessionManager MakeTokenDecryptingManager(
    const opcua::ByteString& server_certificate_der,
    std::shared_ptr<binary::crypto::PrivateKey> server_key,
    std::function<opcua::base::Time()> now) {
  return ServerSessionManager{{
      .authenticator = opcua::MakeCoroutineAuthenticator(
          [](opcua::LocalizedText user_name, opcua::LocalizedText password)
              -> opcua::Awaitable<
                  opcua::StatusOr<opcua::AuthenticationResult>> {
            EXPECT_EQ(user_name, opcua::LocalizedText{u"operator"});
            EXPECT_EQ(password, opcua::LocalizedText{u"secret"});
            co_return opcua::AuthenticationResult{
                .user_id = opcua::NodeId{7, 4}, .multi_sessions = true};
          }),
      .server_certificate = server_certificate_der,
      .decrypt_user_token =
          [server_key = std::move(server_key)](
              std::span<const std::uint8_t> ciphertext) {
            return binary::crypto::RsaOaepDecrypt(*server_key, ciphertext);
          },
      .now = std::move(now),
      .min_timeout = opcua::base::TimeDelta::FromSeconds(10),
  }};
}

class ServerSessionManagerTest : public testing::Test {
 protected:
  opcua::base::Time now_ = opcua::base::Time::Now();
  opcua::TestExecutor executor_;

  ServerSessionManager MakeManager() {
    return ServerSessionManager{{
        .authenticator = opcua::MakeCoroutineAuthenticator(
            [](opcua::LocalizedText user_name, opcua::LocalizedText password)
                -> opcua::Awaitable<
                    opcua::StatusOr<opcua::AuthenticationResult>> {
              EXPECT_EQ(user_name, opcua::LocalizedText{u"operator"});
              EXPECT_EQ(password, opcua::LocalizedText{u"secret"});
              co_return opcua::AuthenticationResult{
                  .user_id = opcua::NodeId{42, 4}, .multi_sessions = true};
            }),
        .now = [this] { return now_; },
        .min_timeout = opcua::base::TimeDelta::FromSeconds(10),
    }};
  }
};

TEST_F(ServerSessionManagerTest, ActivatesDetachesResumesAndClosesSession) {
  auto manager = MakeManager();

  const auto created =
      opcua::WaitAwaitable(executor_, manager.CreateSession({}));
  EXPECT_EQ(created.status.code(), opcua::StatusCode::Good);
  EXPECT_FALSE(created.session_id.is_null());
  EXPECT_FALSE(created.authentication_token.is_null());

  const auto activated = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .user_name = opcua::LocalizedText{u"operator"},
                     .password = opcua::LocalizedText{u"secret"},
                 }));
  EXPECT_EQ(activated.status.code(), opcua::StatusCode::Good);
  EXPECT_FALSE(activated.resumed);
  ASSERT_TRUE(activated.authentication_result.has_value());
  EXPECT_EQ(activated.authentication_result->user_id, opcua::NodeId(42, 4));

  manager.DetachSession(created.authentication_token);
  const auto detached = manager.FindSession(created.authentication_token);
  ASSERT_TRUE(detached.has_value());
  EXPECT_FALSE(detached->attached);

  const auto resumed = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                 }));
  EXPECT_EQ(resumed.status.code(), opcua::StatusCode::Good);
  EXPECT_TRUE(resumed.resumed);

  const auto closed = manager.CloseSession(
      {.session_id = created.session_id,
       .authentication_token = created.authentication_token});
  EXPECT_EQ(closed.status.code(), opcua::StatusCode::Good);
  EXPECT_FALSE(manager.FindSession(created.authentication_token).has_value());
}

// --- Secured session (Basic256Sha256) tests --------------------------------

class ServerSessionManagerSecureTest : public ServerSessionManagerTest {
 protected:
  const opcua::ByteString server_certificate_{'S', 'R', 'V', 'C',
                                              'E', 'R', 'T'};

  ServerSessionManager MakeSecureManager() {
    return ServerSessionManager{{
        .authenticator = opcua::MakeCoroutineAuthenticator(
            [](opcua::LocalizedText, opcua::LocalizedText)
                -> opcua::Awaitable<
                    opcua::StatusOr<opcua::AuthenticationResult>> {
              co_return opcua::AuthenticationResult{.user_id =
                                                        opcua::NodeId{42, 4}};
            }),
        .server_certificate = server_certificate_,
        .now = [this] { return now_; },
        .min_timeout = opcua::base::TimeDelta::FromSeconds(10),
    }};
  }
};

TEST_F(ServerSessionManagerSecureTest, AcceptsValidClientSignature) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();

  const auto created = opcua::WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = client.certificate_der,
                 }));
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(created.server_certificate, server_certificate_);
  EXPECT_EQ(created.server_nonce.size(), 32u);

  const auto signature = SignActivation(
      client.private_key, created.server_certificate, created.server_nonce);
  const auto activated = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .allow_anonymous = true,
                     .client_signature_algorithm =
                         "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256",
                     .client_signature = signature,
                 }));
  EXPECT_EQ(activated.status.code(), opcua::StatusCode::Good);
}

TEST_F(ServerSessionManagerSecureTest, RejectsMissingClientSignature) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();

  const auto created = opcua::WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = client.certificate_der,
                 }));
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);

  const auto activated = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .allow_anonymous = true,
                 }));
  EXPECT_EQ(activated.status.code(),
            opcua::StatusCode::Bad_ApplicationSignatureInvalid);
}

TEST_F(ServerSessionManagerSecureTest, RejectsSignatureOverWrongData) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();

  const auto created = opcua::WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = client.certificate_der,
                 }));
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);

  // Sign over a different nonce than the one bound to the session.
  const auto signature = SignActivation(
      client.private_key, created.server_certificate, opcua::ByteString(32, 0));
  const auto activated = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .allow_anonymous = true,
                     .client_signature = signature,
                 }));
  EXPECT_EQ(activated.status.code(),
            opcua::StatusCode::Bad_ApplicationSignatureInvalid);
}

TEST_F(ServerSessionManagerSecureTest,
       RejectsCreateSessionCertificateMismatch) {
  auto manager = MakeSecureManager();
  const auto client = GenerateClientIdentity();
  const auto other = GenerateClientIdentity();

  // Body certificate differs from the SecureChannel certificate.
  const auto created = opcua::WaitAwaitable(
      executor_, manager.CreateSession({
                     .client_certificate = client.certificate_der,
                     .channel_secure = true,
                     .channel_certificate = other.certificate_der,
                 }));
  EXPECT_EQ(created.status.code(),
            opcua::StatusCode::Bad_ApplicationSignatureInvalid);
}

TEST_F(ServerSessionManagerTest, DecryptsEncryptedUserNameToken) {
  auto server = GenerateClientIdentity();
  auto server_key = std::make_shared<binary::crypto::PrivateKey>(
      std::move(server.private_key));
  auto server_cert =
      binary::crypto::LoadDerCertificate(ByteSpan(server.certificate_der));
  ASSERT_TRUE(server_cert.ok());
  auto server_pub = binary::crypto::CertificatePublicKey(*server_cert);
  ASSERT_TRUE(server_pub.ok());

  auto manager = MakeTokenDecryptingManager(server.certificate_der, server_key,
                                            [this] { return now_; });

  const auto created =
      opcua::WaitAwaitable(executor_, manager.CreateSession({}));
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);

  const auto encrypted =
      EncryptUserToken(*server_pub, "secret", created.server_nonce);
  const auto activated = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .user_name = opcua::LocalizedText{u"operator"},
                     .encrypted_password = encrypted,
                     .password_encryption_algorithm =
                         "http://www.w3.org/2001/04/xmlenc#rsa-oaep",
                 }));
  EXPECT_EQ(activated.status.code(), opcua::StatusCode::Good);
  ASSERT_TRUE(activated.authentication_result.has_value());
  EXPECT_EQ(activated.authentication_result->user_id, opcua::NodeId(7, 4));
}

TEST_F(ServerSessionManagerTest, RejectsEncryptedUserNameTokenWithWrongNonce) {
  auto server = GenerateClientIdentity();
  auto server_key = std::make_shared<binary::crypto::PrivateKey>(
      std::move(server.private_key));
  auto server_cert =
      binary::crypto::LoadDerCertificate(ByteSpan(server.certificate_der));
  ASSERT_TRUE(server_cert.ok());
  auto server_pub = binary::crypto::CertificatePublicKey(*server_cert);
  ASSERT_TRUE(server_pub.ok());

  auto manager = MakeTokenDecryptingManager(server.certificate_der, server_key,
                                            [this] { return now_; });

  const auto created =
      opcua::WaitAwaitable(executor_, manager.CreateSession({}));
  ASSERT_EQ(created.status.code(), opcua::StatusCode::Good);

  // Encrypt over a nonce that does not match the session nonce.
  const auto encrypted =
      EncryptUserToken(*server_pub, "secret", opcua::ByteString(32, 0));
  const auto activated = opcua::WaitAwaitable(
      executor_, manager.ActivateSession({
                     .session_id = created.session_id,
                     .authentication_token = created.authentication_token,
                     .user_name = opcua::LocalizedText{u"operator"},
                     .encrypted_password = encrypted,
                     .password_encryption_algorithm =
                         "http://www.w3.org/2001/04/xmlenc#rsa-oaep",
                 }));
  EXPECT_EQ(activated.status.code(),
            opcua::StatusCode::Bad_WrongLoginCredentials);
}

}  // namespace
}  // namespace opcua
