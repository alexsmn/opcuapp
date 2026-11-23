#include "opcua/client/client_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/scoped_temp_dir.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/session/server_session_manager.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/reverse_connect.h"
#include "opcua/transport/binary/runtime.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/server.h"
#include "opcua/transport/binary/test/test_certificate.h"

#include <transport/tcp_transport.h>
#include <transport/transport_factory_impl.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A real ClientSession against a real binary::Server over loopback TCP with a
// SignAndEncrypt SecureChannel, where the SERVER opens the connection: OPC UA
// Part 6 §7.1.3 Establishing a connection,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/7.1.3. Nothing
// listens on the server's endpoint URL; the client reaches it only through the
// sockets the server's ReverseConnector dials to the client's listener.

namespace opcua {
namespace {

using namespace std::chrono_literals;

constexpr char kApplicationUri[] = "urn:test:reverse-gateway";
// Port 9 (discard) is never an OPC UA listener: a connect that dialled instead
// of taking a reverse socket would fail.
constexpr char kEndpointUrl[] = "opc.tcp://127.0.0.1:9";

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << bytes;
}

bool RunUntil(TestExecutor& executor,
              const std::function<bool()>& done,
              std::chrono::milliseconds limit = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline)
      return false;
    executor.Poll();
    std::this_thread::yield();
  }
  return true;
}

class ClientSessionReverseConnectE2eTest : public ::testing::Test {
 protected:
  void SetUp() override { StartClientListener(); }

  void TearDown() override {
    if (session_)
      WaitAwaitable(executor_, session_->Disconnect());
    session_.reset();
    if (connector_)
      connector_->Stop();
    if (listener_)
      WaitAwaitable(executor_, listener_->Close());
    Drain(executor_);
  }

  // The client half: a listener whose recogniser accepts the gateway, and a
  // pool the session's factory draws from.
  void StartClientListener() {
    auto acceptor = std::make_unique<transport::PassiveTcpTransport>(
        any_executor_, transport::log_source{}, "127.0.0.1", "0");
    auto* acceptor_ptr = acceptor.get();
    listener_.emplace(binary::ReverseConnectListenerContext{
        .acceptor = transport::any_transport{std::move(acceptor)},
        .pool = pool_,
        .recognize =
            [](const binary::ReverseHelloMessage& hello,
               const std::string& key) {
              return hello.server_uri == kApplicationUri &&
                     key == *binary::ReverseConnectKey(kEndpointUrl);
            },
    });
    ASSERT_EQ(WaitAwaitable(executor_, listener_->Open()), transport::OK);
    listener_port_ = acceptor_ptr->GetLocalPort();
  }

  // The server half: SignAndEncrypt endpoints, a session manager, a Server
  // that is never opened, and a ReverseConnector that dials the client.
  void StartGateway(std::string application_uri = kApplicationUri) {
    server_keys_ = binary::test::GenerateSelfSignedRsa(
        {.uris = {application_uri},
         .dns_names = certificate_dns_names_,
         .ip_addresses = certificate_ip_addresses_});
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

    std::vector<EndpointDescription> endpoints{EndpointDescription{
        .endpoint_url = kEndpointUrl,
        .server = {.application_uri = application_uri,
                   .application_type = ApplicationType::Server},
        .server_certificate = secure_config_->certificate_der,
        .security_mode = MessageSecurityMode::SignAndEncrypt,
        .security_policy_uri =
            std::string{binary::kSecurityPolicyBasic256Sha256},
        .user_identity_tokens = {{.policy_id = "anonymous",
                                  .token_type = UserTokenType::Anonymous}},
        .transport_profile_uri =
            "http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary",
        .security_level = 1,
    }};
    session_manager_.emplace(ServerSessionManagerContext{
        .authenticator = MakeCoroutineAuthenticator(
            [](LocalizedText,
               LocalizedText) -> CoStatusOr<AuthenticationResult> {
              co_return StatusOr<AuthenticationResult>{
                  Status{StatusCode::Bad_IdentityTokenRejected}};
            }),
        .server_certificate = secure_config_->certificate_der,
        .anonymous_only = true,
    });
    ServiceCallbacks callbacks;
    callbacks.read = [this](
                         ServiceContext,
                         std::shared_ptr<const std::vector<ReadValueId>> inputs)
        -> CoStatusOr<std::vector<DataValue>> {
      ++reads_;
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
    server_.emplace(binary::ServerContext{
        .acceptor =
            transport::any_transport{
                std::make_unique<transport::PassiveTcpTransport>(
                    any_executor_, transport::log_source{}, "127.0.0.1", "0")},
        .runtime = *runtime_,
        .session_manager = *session_manager_,
        .secure_channel_config = secure_config_});

    connector_.emplace(binary::ReverseConnectorContext{
        .executor = any_executor_,
        .transport_factory = transport_factory_,
        .client_url = "opc.tcp://127.0.0.1:" + std::to_string(listener_port_),
        .hello = {.server_uri = std::move(application_uri),
                  .endpoint_url = kEndpointUrl},
        .serve = [this](transport::any_transport socket) -> Awaitable<void> {
          co_await server_->ServeConnection(std::move(socket));
        },
        .retry_delay = 100ms,
        .min_backoff = 10ms,
    });
    connector_->Start();
  }

  SessionConnectParams Params(bool trust_gateway) {
    const auto client_keys = binary::test::GenerateSelfSignedRsa(
        {.common_name = "proxy", .uris = {"urn:test:proxy"}});
    WriteFile(dir_.path() / "client.pem", client_keys.cert_pem);
    WriteFile(dir_.path() / "client.key", client_keys.private_key_pem);
    SessionConnectParams params;
    params.connection_string = kEndpointUrl;
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
    if (trust_gateway)
      WriteFile(dir_.path() / "trusted" / "gateway.pem", server_keys_.cert_pem);
    return params;
  }

  Status Connect(SessionConnectParams params) {
    if (!session_) {
      session_ = std::make_shared<ClientSession>(AnyExecutor{executor_},
                                                 reverse_factory_);
    }
    return WaitAwaitable(executor_, session_->ConnectStatus(std::move(params)));
  }

  bool WaitForIdleSocket() {
    return RunUntil(executor_, [this] {
      return pool_.idle_count(*binary::ReverseConnectKey(kEndpointUrl)) > 0;
    });
  }

  test::ScopedTempDir dir_{"opcua_reverse_e2e"};
  TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  transport::TransportFactoryImpl transport_factory_;

  // Client side.
  binary::ReverseConnectPool pool_;
  binary::ReverseConnectTransportFactory reverse_factory_{transport_factory_,
                                                          pool_, 5s};
  std::optional<binary::ReverseConnectListener> listener_;
  int listener_port_ = 0;
  std::shared_ptr<ClientSession> session_;

  // Server side.
  binary::test::PemKeypair server_keys_;
  std::shared_ptr<const binary::SecureChannelServerConfig> secure_config_;
  std::optional<ServerSessionManager> session_manager_;
  std::optional<binary::Runtime> runtime_;
  std::optional<binary::Server> server_;
  std::optional<binary::ReverseConnector> connector_;
  int reads_ = 0;
  std::vector<std::string> certificate_dns_names_;
  std::vector<std::string> certificate_ip_addresses_{"127.0.0.1"};
};

TEST_F(ClientSessionReverseConnectE2eTest,
       ConnectsOverTheSocketTheServerDialled) {
  StartGateway();
  ASSERT_TRUE(WaitForIdleSocket());

  EXPECT_EQ(Connect(Params(/*trust_gateway=*/true)).code(), StatusCode::Good);
  EXPECT_TRUE(session_->IsConnected());
  // The session read the NamespaceArray over the reverse connection.
  EXPECT_GE(reads_, 1);
  // The connector replaced each socket the client took: discovery's, then the
  // session's.
  EXPECT_GE(connector_->engaged_count(), 1u);
  EXPECT_TRUE(WaitForIdleSocket());
}

TEST_F(ClientSessionReverseConnectE2eTest, ReconnectsOnAFreshReverseSocket) {
  StartGateway();
  ASSERT_TRUE(WaitForIdleSocket());
  ASSERT_EQ(Connect(Params(/*trust_gateway=*/true)).code(), StatusCode::Good);

  WaitAwaitable(executor_, session_->Disconnect());
  EXPECT_FALSE(session_->IsConnected());
  ASSERT_TRUE(WaitForIdleSocket());
  EXPECT_EQ(Connect(Params(/*trust_gateway=*/true)).code(), StatusCode::Good);
  EXPECT_TRUE(session_->IsConnected());
}

// The ReverseHello authenticates nothing: the gateway's certificate is still
// checked against the client's trust store.
TEST_F(ClientSessionReverseConnectE2eTest, AnUntrustedGatewayIsRefused) {
  StartGateway();
  ASSERT_TRUE(WaitForIdleSocket());
  EXPECT_EQ(Connect(Params(/*trust_gateway=*/false)).code(),
            StatusCode::Bad_CertificateUntrusted);
  EXPECT_FALSE(std::filesystem::is_empty(dir_.path() / "rejected"));
}

// The host the ReverseHello's EndpointUrl names is the host the gateway's
// certificate must carry (OPC UA Part 4 §6.1.3 Determining if a Certificate is
// trusted, https://reference.opcfoundation.org/Core/Part4/v105/docs/6.1.3), so
// a trusted certificate issued for another computer cannot claim this one.
TEST_F(ClientSessionReverseConnectE2eTest,
       AGatewayCertificateForAnotherHostIsRefused) {
  certificate_dns_names_ = {"elsewhere.example"};
  certificate_ip_addresses_ = {};
  StartGateway();
  ASSERT_TRUE(WaitForIdleSocket());
  EXPECT_EQ(Connect(Params(/*trust_gateway=*/true)).code(),
            StatusCode::Bad_CertificateHostNameInvalid);
}

}  // namespace
}  // namespace opcua
