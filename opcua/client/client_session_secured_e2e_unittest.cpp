#include "opcua/client/client_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/scoped_temp_dir.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/client/endpoint_selection.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/session/server_session_manager.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/runtime.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/server.h"
#include "opcua/transport/binary/test/test_certificate.h"

#include <transport/tcp_transport.h>
#include <transport/transport_factory_impl.h>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A real ClientSession against a real binary::Server over loopback TCP, with a
// SignAndEncrypt SecureChannel. Until this existed no test anywhere completed a
// secured session end to end: the secure-channel tests drive the two channel
// halves directly, and every E2E suite connects with SecurityPolicy=None.

namespace opcua {
namespace {

constexpr char kApplicationUri[] = "urn:test:secured-server";

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << bytes;
}

ByteString DerOf(const std::string& pem) {
  auto certificate = binary::crypto::LoadPemCertificate(pem);
  EXPECT_TRUE(certificate.ok());
  auto der = binary::crypto::CertificateDer(*certificate);
  EXPECT_TRUE(der.ok());
  return std::move(*der);
}

UserTokenPolicy AnonymousPolicy() {
  return {.policy_id = "anonymous", .token_type = UserTokenType::Anonymous};
}

UserTokenPolicy UserNamePolicy(std::string_view security_policy) {
  return {.policy_id = "username",
          .token_type = UserTokenType::UserName,
          .security_policy_uri = std::string{security_policy}};
}

struct ServerOptions {
  binary::test::TestCertificateOptions certificate{
      .uris = {kApplicationUri},
      .ip_addresses = {"127.0.0.1"}};
  bool anonymous_only = false;
};

class ClientSessionSecuredE2eTest : public ::testing::Test {
 protected:
  // Starts a listener on 127.0.0.1 with SignAndEncrypt endpoints under both RSA
  // policies.
  void StartServer(ServerOptions options = {}) {
    server_keys_ = binary::test::GenerateSelfSignedRsa(options.certificate);
    auto certificate =
        binary::crypto::LoadPemCertificate(server_keys_.cert_pem);
    auto private_key =
        binary::crypto::LoadPemPrivateKey(server_keys_.private_key_pem);
    ASSERT_TRUE(certificate.ok());
    ASSERT_TRUE(private_key.ok());
    auto config = binary::MakeSecureChannelServerConfig(
        std::move(*certificate), std::move(*private_key),
        /*allow_none=*/false, {});
    ASSERT_TRUE(config.ok());
    secure_config_ = std::move(*config);

    std::vector<UserTokenPolicy> tokens{AnonymousPolicy()};
    std::vector<EndpointDescription> endpoints;
    for (const auto policy : {binary::kSecurityPolicyBasic256Sha256,
                              binary::kSecurityPolicyAes128Sha256RsaOaep}) {
      std::vector<UserTokenPolicy> endpoint_tokens{AnonymousPolicy()};
      if (!options.anonymous_only)
        endpoint_tokens.push_back(UserNamePolicy(policy));
      endpoints.push_back(EndpointDescription{
          .endpoint_url = "opc.tcp://127.0.0.1:0",
          .server = {.application_uri = kApplicationUri,
                     .application_type = ApplicationType::Server},
          .server_certificate = secure_config_->certificate_der,
          .security_mode = MessageSecurityMode::SignAndEncrypt,
          .security_policy_uri = std::string{policy},
          .user_identity_tokens = std::move(endpoint_tokens),
          .transport_profile_uri =
              "http://opcfoundation.org/UA-Profile/Transport/"
              "uatcp-uasc-uabinary",
          .security_level = 1,
      });
    }

    session_manager_.emplace(ServerSessionManagerContext{
        .authenticator = MakeCoroutineAuthenticator(
            [this](LocalizedText user,
                   LocalizedText password) -> CoStatusOr<AuthenticationResult> {
              received_password_ = password;
              if (user != LocalizedText{u"operator"} ||
                  password != LocalizedText{u"s3cret"}) {
                co_return StatusOr<AuthenticationResult>{
                    Status{StatusCode::Bad_IdentityTokenRejected}};
              }
              co_return AuthenticationResult{.user_id = NodeId{5, 2},
                                             .multi_sessions = true};
            }),
        .server_certificate = secure_config_->certificate_der,
        .decrypt_user_token =
            [this](std::span<const std::uint8_t> ciphertext) {
              ++decrypted_tokens_;
              return binary::crypto::RsaOaepDecrypt(secure_config_->private_key,
                                                    ciphertext);
            },
        // The token must travel encrypted even on a SignAndEncrypt channel.
        .require_encryption_for_password = true,
        .anonymous_only = options.anonymous_only,
    });
    // The client reads the NamespaceArray straight after activating; without
    // an answer it waits out its request timeout.
    ServiceCallbacks callbacks;
    callbacks.read = [](ServiceContext,
                        std::shared_ptr<const std::vector<ReadValueId>> inputs)
        -> CoStatusOr<std::vector<DataValue>> {
      co_return std::vector<DataValue>(
          inputs->size(),
          MakeReadResult(std::vector<String>{"http://opcfoundation.org/UA/"}));
    };
    runtime_.emplace(
        binary::RuntimeContext{.executor = AnyExecutor{executor_},
                               .session_manager = *session_manager_,
                               .callbacks = std::move(callbacks),
                               .endpoints = std::move(endpoints),
                               .require_secure_channel_for_sessions = true});

    auto acceptor = std::make_unique<transport::PassiveTcpTransport>(
        any_executor_, transport::log_source{}, "127.0.0.1", "0");
    auto* acceptor_ptr = acceptor.get();
    server_.emplace(binary::ServerContext{
        .acceptor = transport::any_transport{std::move(acceptor)},
        .runtime = *runtime_,
        .session_manager = *session_manager_,
        .secure_channel_config = secure_config_});
    ASSERT_EQ(WaitAwaitable(executor_, server_->Open()), transport::OK);
    port_ = acceptor_ptr->GetLocalPort();
  }

  void TearDown() override {
    if (session_)
      WaitAwaitable(executor_, session_->Disconnect());
    session_.reset();
    if (server_)
      (void)WaitAwaitable(executor_, server_->Close());
  }

  // Client settings with its own certificate; `trust_server` additionally
  // provisions the server's certificate as trusted.
  SessionConnectParams Params(bool trust_server) {
    const auto client_keys = binary::test::GenerateSelfSignedRsa(
        {.common_name = "client", .uris = {"urn:test:client"}});
    WriteFile(dir_.path() / "client.pem", client_keys.cert_pem);
    WriteFile(dir_.path() / "client.key", client_keys.private_key_pem);
    SessionConnectParams params;
    params.host = "127.0.0.1:" + std::to_string(port_);
    params.security.mode = SessionSecuritySettings::Mode::SignAndEncrypt;
    params.security.client_certificate_path =
        (dir_.path() / "client.pem").string();
    params.security.client_private_key_path =
        (dir_.path() / "client.key").string();
    std::filesystem::create_directories(dir_.path() / "trusted");
    params.security.trusted_certificates_dir =
        (dir_.path() / "trusted").string();
    params.security.rejected_certificates_dir =
        (dir_.path() / "rejected").string();
    if (trust_server)
      WriteFile(dir_.path() / "trusted" / "server.pem", server_keys_.cert_pem);
    return params;
  }

  Status Connect(SessionConnectParams params) {
    session_ = std::make_shared<ClientSession>(AnyExecutor{executor_},
                                               transport_factory_);
    return WaitAwaitable(executor_, session_->ConnectStatus(std::move(params)));
  }

  test::ScopedTempDir dir_{"opcua_secured_e2e"};
  TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  transport::TransportFactoryImpl transport_factory_;

  binary::test::PemKeypair server_keys_;
  std::shared_ptr<const binary::SecureChannelServerConfig> secure_config_;
  std::optional<ServerSessionManager> session_manager_;
  std::optional<binary::Runtime> runtime_;
  std::optional<binary::Server> server_;
  int port_ = 0;

  std::shared_ptr<ClientSession> session_;
  std::optional<LocalizedText> received_password_;
  int decrypted_tokens_ = 0;
};

TEST_F(ClientSessionSecuredE2eTest, TrustedServerAndEncryptedPassword) {
  StartServer();
  auto params = Params(/*trust_server=*/true);
  params.user_name = u"operator";
  params.password = u"s3cret";

  EXPECT_EQ(Connect(std::move(params)).code(), StatusCode::Good);
  EXPECT_TRUE(session_->IsConnected());
  // The password crossed the wire inside an encrypted token secret (Part 4
  // §7.40.2.2), and the server recovered it.
  EXPECT_EQ(decrypted_tokens_, 1);
  EXPECT_EQ(received_password_, LocalizedText{u"s3cret"});
}

TEST_F(ClientSessionSecuredE2eTest, Aes128Sha256RsaOaepSession) {
  StartServer();
  auto params = Params(/*trust_server=*/true);
  params.security.required_policy_uri =
      std::string{kSecurityPolicyUriAes128Sha256RsaOaep};
  params.user_name = u"operator";
  params.password = u"s3cret";

  EXPECT_EQ(Connect(std::move(params)).code(), StatusCode::Good);
  EXPECT_EQ(decrypted_tokens_, 1);
}

TEST_F(ClientSessionSecuredE2eTest, AnonymousSessionNeedsNoPassword) {
  StartServer();
  EXPECT_EQ(Connect(Params(/*trust_server=*/true)).code(), StatusCode::Good);
  EXPECT_EQ(decrypted_tokens_, 0);
}

TEST_F(ClientSessionSecuredE2eTest, UntrustedServerIsRefusedAndKept) {
  StartServer();
  EXPECT_EQ(Connect(Params(/*trust_server=*/false)).code(),
            StatusCode::Bad_CertificateUntrusted);
  // The refused certificate lands in the rejected directory, where an operator
  // -- or a trust prompt -- can promote it.
  EXPECT_FALSE(std::filesystem::is_empty(dir_.path() / "rejected"));
}

TEST_F(ClientSessionSecuredE2eTest, CertificateForAnotherHostIsRefused) {
  StartServer({.certificate = {.uris = {kApplicationUri},
                               .dns_names = {"elsewhere.example"}}});
  EXPECT_EQ(Connect(Params(/*trust_server=*/true)).code(),
            StatusCode::Bad_CertificateHostNameInvalid);
}

TEST_F(ClientSessionSecuredE2eTest, CertificateForAnotherApplicationIsRefused) {
  StartServer({.certificate = {.uris = {"urn:test:some-other-app"},
                               .ip_addresses = {"127.0.0.1"}}});
  EXPECT_EQ(Connect(Params(/*trust_server=*/true)).code(),
            StatusCode::Bad_CertificateUriInvalid);
}

TEST_F(ClientSessionSecuredE2eTest,
       UserNameIsNotSentToAnAnonymousOnlyEndpoint) {
  StartServer({.anonymous_only = true});
  auto params = Params(/*trust_server=*/true);
  params.user_name = u"operator";
  params.password = u"s3cret";

  EXPECT_EQ(Connect(std::move(params)).code(),
            StatusCode::Bad_IdentityTokenRejected);
  // Refused by the client before ActivateSession: the server never saw it.
  EXPECT_FALSE(received_password_.has_value());
  EXPECT_EQ(decrypted_tokens_, 0);
}

// The server still answers discovery over an unsecured channel (OPC UA Part 4
// §5.5.1, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.5.1) --
// the secured cases above depend on it -- but a session over one is refused.
TEST_F(ClientSessionSecuredE2eTest, UnsecuredSessionIsRefused) {
  StartServer();
  SessionConnectParams params;
  params.host = "127.0.0.1:" + std::to_string(port_);
  EXPECT_EQ(Connect(std::move(params)).code(),
            StatusCode::Bad_SecurityModeInsufficient);
}

}  // namespace
}  // namespace opcua
