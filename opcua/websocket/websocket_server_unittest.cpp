#include "opcua/websocket/json_codec.h"
#include "opcua/websocket/server.h"

#include "opcua/base/any_executor.h"
#include "opcua/scada/attribute_service_mock.h"
#include "opcua/scada/authentication_adapters.h"
#include "opcua/scada/history_service_mock.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/method_service_mock.h"
#include "opcua/scada/node_management_service_mock.h"
#include "opcua/scada/test/test_monitored_item.h"
#include "opcua/scada/view_service_mock.h"
#include "transport/websocket_transport.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <thread>

using namespace testing;

namespace opcua::ws {
namespace {

namespace http = boost::beast::http;

constexpr auto kTestCertificatePem = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUQWAR+40WE34MoTuKigrGeiGT5ycwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQyMDE4NTczMloXDTM2MDQx
NzE4NTczMlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAsUhr1SNMP7HMVoUgPK1j2ecIvRSz0PiGagh5kOL6HETy
Eo3MTux9LheWcvhhMvpm3HNOIx6Q8iModo77le+nyYxExrQ4xNFTvwsFhkpso2sQ
C9fWO7uabAFhMSJZZq5vs8y4o3aMQNTeSBWzkoX0ebdzkBn0YbLa6Gvas87oF50g
NWiOKJNNmZL6rC2iSMI+2c/fq7UrPkmDd6VObFXO7ItTFXbqeXPiynuMnLXf5sGU
V3VKP8PyVGR7QJq7LRa30BCsfSTAK6BlVIcPkDMA+1uVM6lkPI103axRF3UhwwU6
B8WOpyafhul18ZiFtSYz78x5K3kYrSvYjOOb1YeHEwIDAQABo1MwUTAdBgNVHQ4E
FgQUaGBbcBpQLHhmkVgc1Eg2OOXTLM0wHwYDVR0jBBgwFoAUaGBbcBpQLHhmkVgc
1Eg2OOXTLM0wDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAeEkK
63ra3vPoBp49UWiRgPfroFKbX/AI01/r0WRsSDzwOPoKRmSpuwdcXfT3B3ilzmDX
sfiAJJQtvcgm3V3vNzWmks+4dLS0G+bRwBxih5b16xAWbUcUgty9TX3HOlAzqdOh
BnoQQczubSQv2/0AzfxZ06EGpwrlFrTzXJRpmQRoqo8xGEwgiQ10A3aXYYyxydzj
ChS5hFDZ4P6MA01VI2y5dgIUiA+3uEWOt2SJgexDaFk7ZmpVqzh4KHV0deSyG3Gt
QoPGNJO4DZVyCXKd4iOg40H4JRc2fIlMykOklAD7ukuUpGNeEPKzSQjakxKYKiX8
NGfJbg7n4a46yf+S6A==
-----END CERTIFICATE-----
)";

constexpr auto kTestPrivateKeyPem = R"(-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCxSGvVI0w/scxW
hSA8rWPZ5wi9FLPQ+IZqCHmQ4vocRPISjcxO7H0uF5Zy+GEy+mbcc04jHpDyIyh2
jvuV76fJjETGtDjE0VO/CwWGSmyjaxAL19Y7u5psAWExIllmrm+zzLijdoxA1N5I
FbOShfR5t3OQGfRhstroa9qzzugXnSA1aI4ok02ZkvqsLaJIwj7Zz9+rtSs+SYN3
pU5sVc7si1MVdup5c+LKe4yctd/mwZRXdUo/w/JUZHtAmrstFrfQEKx9JMAroGVU
hw+QMwD7W5UzqWQ8jXTdrFEXdSHDBToHxY6nJp+G6XXxmIW1JjPvzHkreRitK9iM
45vVh4cTAgMBAAECggEAD+7UUimD9s2B8dyxEwL6UGElNekgaA2N9wWf91eO5u+D
WguIaydx8KyKBvcvtScwC2wJf7qFiF2Ei3M6RTVuvPxwSfN0jqvJfQf+jR0vOliq
7oWNaXzo2gAdvg66PjI7M8uYZIiI/mKjP5NDuk1ztWS5bCAJCKbMacsXssVLsqN0
ENH+/vLsawjvD+/UQQGr4FUvw0QCYz1FA5C4XCyHEidMORRSOhh7yWB38lbO0StU
StnWGdeYxNz1+VGJwfckuvvKVl8A6vY8So5LPsHS2aqDXcAwmSkJgd78k/eaW1V7
bQ7kn8NtXECrphIYcHu4/yuvwp+Y9Avc3pCtC4etQQKBgQDEjupDiFsOM3RDNfoj
9fQc/jC9I61TBoi9kWIj0UMR1nCi+SNlxm5uV4gRQcTs4MSxFGqgcdCm8Bb5MNld
dxsMJYOOqDSciMpyEdwCEtk17ctR48+Vsan22xxzqezsI7Skvzu0FzBs7g5cnE79
hxeV3+qgrirnO7xwe6dJLrOCIwKBgQDm5UB+kS9SWc31Dhxc0fuOhDa7PhDQKo4V
ZsCEtc7hpytcjrLSC9Ru5Er1tp3L/oF5jQLMalhdMR1QagG7cJOAdT4K3+tjnags
E4c/Vw5xzRdjuWZEAleZATyRwonvFXSf868Sg8op6xZjMFq4qPb+8mJexzinVOM9
ipsA2CveUQKBgQDCosJXHS8NYOY/p7OK6IJSM2MP58Q58r50+QG1dgJ0J2Rh/VKP
9W5k1UhnzjiyV+BteUoclpeGtzgIida0Nr0RyhP7r5RpbQsK6aRyaTetr0smS+/C
y6sCRvZlkl6JdtHqUXNNYakSNKkEC8QsSRmRz6kGc3EIiJ6Qw+FjFlurAQKBgQDZ
PWch7j3E0IPMBfu/hT2WiGTqZOnywacvEZ8e/ePpQZy1l/k9US4NK7QvXSM4VHvD
Pl4csA31mIlJKIP6tF/DZAv8tVNGRYZ9+d2tRZ5sihdwl3ZVlJKQfa5cQdn/XYN+
HwtgcyjZqbtFlbA1v5usoabWH8D5BxBKzccq0zjrEQKBgQCaSvtY2PveeWHZBOlN
aFTbyLdyBHdhwpYZm3vKQ3MoOtnQGRq1BlQk7ojruxGhKMTnJhNP2cSnDdaUJB/S
urjYLs2lpMW81+/p1qt5QCP+hVU+/Y7rdCXmTQjsy+9rAf3VOupMisj2gmgX8ASm
mCzxKIlbzMnhGhGlzdKwqs5Uhw==
-----END PRIVATE KEY-----
)";

scada::NodeId NumericNode(scada::NumericId id, scada::NamespaceIndex ns = 2) {
  return {id, ns};
}

std::string Trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return std::string{value};
}

bool HeaderContainsToken(std::string_view header, std::string_view token) {
  while (!header.empty()) {
    const auto comma = header.find(',');
    const auto part =
        comma == std::string_view::npos ? header : header.substr(0, comma);
    if (Trim(part) == token)
      return true;
    if (comma == std::string_view::npos)
      break;
    header.remove_prefix(comma + 1);
  }
  return false;
}

class TestMonitoredItemService : public scada::MonitoredItemService {
 public:
  std::shared_ptr<scada::MonitoredItem> CreateMonitoredItem(
      const scada::ReadValueId&,
      const scada::MonitoringParameters&) {
    return std::make_shared<scada::TestMonitoredItem>();
  }

  scada::StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
  CreateSubscription(scada::ServiceContext /*context*/,
                     scada::MonitoredItemSubscriptionOptions options) override {
    return scada::MakeItemFactorySubscription(
        [this](const scada::ReadValueId& value_id,
               const scada::MonitoringParameters& params) {
          return CreateMonitoredItem(value_id, params);
        },
        options);
  }
};

class BeastClient {
 public:
  void Connect(const std::string& host,
               unsigned short port,
               const std::string& origin,
               const std::string& subprotocol) {
    const auto results = resolver_.resolve(host, std::to_string(port));
    boost::beast::get_lowest_layer(websocket_).connect(results);
    websocket_.set_option(boost::beast::websocket::stream_base::decorator(
        [origin, subprotocol](boost::beast::websocket::request_type& request) {
          if (!origin.empty())
            request.set(boost::beast::http::field::origin, origin);
          if (!subprotocol.empty()) {
            request.set(boost::beast::http::field::sec_websocket_protocol,
                        subprotocol);
          }
        }));
    websocket_.handshake(host + ":" + std::to_string(port), "/ua");
  }

  void Send(const RequestMessage& request) {
    const auto payload = boost::json::serialize(EncodeJson(request));
    websocket_.text(true);
    websocket_.write(boost::asio::buffer(payload));
  }

  std::string Read(std::chrono::milliseconds timeout = std::chrono::seconds{
                       30}) {
    boost::beast::get_lowest_layer(websocket_).expires_after(timeout);
    boost::beast::flat_buffer buffer;
    websocket_.read(buffer);
    boost::beast::get_lowest_layer(websocket_).expires_never();
    return boost::beast::buffers_to_string(buffer.data());
  }

  std::string Request(const RequestMessage& request) {
    Send(request);
    return Read();
  }

  void Close() {
    if (websocket_.is_open())
      websocket_.close(boost::beast::websocket::close_code::normal);
  }

 private:
  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::resolver resolver_{io_context_};
  boost::beast::websocket::stream<boost::beast::tcp_stream> websocket_{
      io_context_};
};

class TlsBeastClient {
 public:
  TlsBeastClient() : websocket_{io_context_, ssl_context_} {
    ssl_context_.set_verify_mode(boost::asio::ssl::verify_none);
  }

  void Connect(const std::string& host,
               unsigned short port,
               const std::string& origin,
               const std::string& subprotocol) {
    const auto results = resolver_.resolve(host, std::to_string(port));
    boost::asio::connect(websocket_.next_layer().next_layer(), results);
    websocket_.next_layer().handshake(boost::asio::ssl::stream_base::client);
    websocket_.set_option(boost::beast::websocket::stream_base::decorator(
        [origin, subprotocol](boost::beast::websocket::request_type& request) {
          if (!origin.empty())
            request.set(boost::beast::http::field::origin, origin);
          if (!subprotocol.empty()) {
            request.set(boost::beast::http::field::sec_websocket_protocol,
                        subprotocol);
          }
        }));
    websocket_.handshake(host + ":" + std::to_string(port), "/ua");
  }

  std::string Request(const RequestMessage& request) {
    const auto payload = boost::json::serialize(EncodeJson(request));
    websocket_.text(true);
    websocket_.write(boost::asio::buffer(payload));

    boost::beast::flat_buffer buffer;
    websocket_.read(buffer);
    return boost::beast::buffers_to_string(buffer.data());
  }

  void Close() {
    if (websocket_.is_open())
      websocket_.close(boost::beast::websocket::close_code::normal);
  }

 private:
  boost::asio::io_context io_context_;
  boost::asio::ssl::context ssl_context_{boost::asio::ssl::context::tls_client};
  boost::asio::ip::tcp::resolver resolver_{io_context_};
  boost::beast::websocket::stream<
      boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>
      websocket_;
};

template <typename TClient>
void ExpectBrowsePagingRoundTrip(TClient& client) {
  const auto create_response = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 1, .body = CreateSessionRequest{}})));
  const auto* created =
      std::get_if<CreateSessionResponse>(&create_response.body);
  ASSERT_NE(created, nullptr);
  EXPECT_EQ(created->status.code(), scada::StatusCode::Good);

  const auto activate_response = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 2,
                      .body = ActivateSessionRequest{
                          .session_id = created->session_id,
                          .authentication_token = created->authentication_token,
                          .user_name = scada::LocalizedText{u"operator"},
                          .password = scada::LocalizedText{u"secret"}}})));
  const auto* activated =
      std::get_if<ActivateSessionResponse>(&activate_response.body);
  ASSERT_NE(activated, nullptr);
  EXPECT_EQ(activated->status.code(), scada::StatusCode::Good);

  const auto browse_response = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 3,
                      .body = BrowseRequest{
                          .requested_max_references_per_node = 2,
                          .inputs = {{.node_id = NumericNode(80),
                                      .direction = scada::BrowseDirection::Both,
                                      .reference_type_id = NumericNode(88),
                                      .include_subtypes = true}}}})));
  const auto* browse = std::get_if<BrowseResponse>(&browse_response.body);
  ASSERT_NE(browse, nullptr);
  ASSERT_EQ(browse->results.size(), 1u);
  ASSERT_EQ(browse->results[0].references.size(), 2u);
  ASSERT_FALSE(browse->results[0].continuation_point.empty());

  const auto browse_next_response = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 4,
                      .body = BrowseNextRequest{
                          .continuation_points = {
                              browse->results[0].continuation_point}}})));
  const auto* browse_next =
      std::get_if<BrowseNextResponse>(&browse_next_response.body);
  ASSERT_NE(browse_next, nullptr);
  ASSERT_EQ(browse_next->results.size(), 1u);
  ASSERT_EQ(browse_next->results[0].references.size(), 1u);
  EXPECT_EQ(browse_next->results[0].references[0].node_id, NumericNode(86));
}

class WebSocketServerTest : public Test {
 protected:
  void SetUp() override {
    work_.emplace(boost::asio::make_work_guard(io_context_));
    thread_.emplace([this] { io_context_.run(); });
  }

  void TearDown() override {
    if (server_) {
      auto close_future = boost::asio::co_spawn(io_context_, server_->Close(),
                                                boost::asio::use_future);
      EXPECT_EQ(close_future.get().value(), 0);
      server_.reset();
      acceptor_ = nullptr;
    }
    runtime_.reset();
    callback_executor_ = AnyExecutor{};
    work_.reset();
    io_context_.stop();
    if (thread_)
      thread_->join();
  }

  void StartServer(
      std::optional<transport::WebSocketServerTlsConfig> tls = std::nullopt) {
    callback_executor_ = io_context_.get_executor();
    runtime_.emplace(ServerRuntimeContext{
        .executor = callback_executor_,
        .session_manager = session_manager_,
        .monitored_item_service = monitored_item_service_,
        .attribute_service = attribute_service_,
        .view_service = view_service_,
        .history_service = history_service_,
        .method_service = method_service_,
        .node_management_service = node_management_service_,
    });
    auto acceptor = std::make_unique<transport::WebSocketTransport>(
        io_context_.get_executor(), transport::log_source{}, "127.0.0.1", "0",
        /*active=*/false,
        transport::WebSocketServerOptions{
            .tls = std::move(tls),
            .handshake_callback =
                [](const transport::WebSocketServerRequest& request)
                -> std::optional<transport::WebSocketServerReject> {
              if (request.target() != "/ua") {
                return transport::WebSocketServerReject{
                    .status = http::status::not_found,
                    .body = "Invalid target",
                };
              }

              const auto subprotocol_it =
                  request.find(http::field::sec_websocket_protocol);
              if (subprotocol_it == request.end() ||
                  !HeaderContainsToken(subprotocol_it->value(),
                                       "opcua+uajson")) {
                return transport::WebSocketServerReject{
                    .status = http::status::bad_request,
                    .body = "Missing required subprotocol",
                };
              }

              const auto origin_it = request.find(http::field::origin);
              if (origin_it == request.end()) {
                return transport::WebSocketServerReject{
                    .status = http::status::forbidden,
                    .body = "Origin required",
                };
              }

              if (origin_it->value() != "https://scada.local") {
                return transport::WebSocketServerReject{
                    .status = http::status::forbidden,
                    .body = "Origin denied",
                };
              }

              return std::nullopt;
            },
            .enable_permessage_deflate = true,
            .response_callback =
                [](boost::beast::websocket::response_type& response) {
                  response.set(http::field::server, "scada-opcua-ws");
                  response.set(http::field::sec_websocket_protocol,
                               "opcua+uajson");
                },
        });
    acceptor_ = acceptor.get();
    server_.emplace(ServerContext{
        .acceptor = transport::any_transport{std::move(acceptor)},
        .runtime = *runtime_,
        .max_message_size = 4 * 1024 * 1024,
    });
    auto open_future = boost::asio::co_spawn(io_context_, server_->Open(),
                                             boost::asio::use_future);
    EXPECT_EQ(open_future.get(), transport::OK);
  }

  unsigned short port() const { return acceptor_->local_endpoint().port(); }

  boost::asio::io_context io_context_;
  std::optional<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_;
  std::optional<std::jthread> thread_;

  NiceMock<scada::MockAttributeService> attribute_service_;
  NiceMock<scada::MockViewService> view_service_;
  NiceMock<scada::MockHistoryService> history_service_;
  NiceMock<scada::MockMethodService> method_service_;
  NiceMock<scada::MockNodeManagementService> node_management_service_;
  TestMonitoredItemService monitored_item_service_;
  AnyExecutor callback_executor_;
  ServerSessionManager session_manager_{
      {.authenticator = scada::MakeCoroutineAuthenticator(
           [](scada::LocalizedText, scada::LocalizedText)
               -> Awaitable<scada::StatusOr<scada::AuthenticationResult>> {
             co_return scada::AuthenticationResult{
                 .user_id = NumericNode(700, 5), .multi_sessions = true};
           })}};
  std::optional<ServerRuntime> runtime_;
  transport::WebSocketTransport* acceptor_ = nullptr;
  std::optional<Server> server_;
};

TEST_F(WebSocketServerTest,
       AcceptsValidHandshakeAndRoutesBrowsePagingEndToEnd) {
  StartServer();

  EXPECT_CALL(view_service_, Browse(_, _))
      .WillOnce(Invoke(
          [&](scada::ServiceContext context,
              std::vector<scada::BrowseDescription> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>> {
            EXPECT_EQ(context.user_id(), NumericNode(700, 5));
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u)
              co_return scada::Status{scada::StatusCode::Bad};
            co_return std::vector{scada::BrowseResult{
                .status_code = scada::StatusCode::Good,
                .references = {{.reference_type_id = NumericNode(81),
                                .forward = true,
                                .node_id = NumericNode(82)},
                               {.reference_type_id = NumericNode(83),
                                .forward = true,
                                .node_id = NumericNode(84)},
                               {.reference_type_id = NumericNode(85),
                                .forward = false,
                                .node_id = NumericNode(86)}}}};
          }));

  BeastClient client;
  client.Connect("127.0.0.1", port(), "https://scada.local", "opcua+uajson");
  ExpectBrowsePagingRoundTrip(client);
  client.Close();
}

TEST_F(WebSocketServerTest, AcceptsTlsHandshakeAndRoutesBrowsePagingEndToEnd) {
  StartServer(transport::WebSocketServerTlsConfig{
      .certificate_chain_pem = kTestCertificatePem,
      .private_key_pem = kTestPrivateKeyPem,
  });

  EXPECT_CALL(view_service_, Browse(_, _))
      .WillOnce(Invoke(
          [&](scada::ServiceContext context,
              std::vector<scada::BrowseDescription> inputs)
              -> Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>> {
            EXPECT_EQ(context.user_id(), NumericNode(700, 5));
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u)
              co_return scada::Status{scada::StatusCode::Bad};
            co_return std::vector{scada::BrowseResult{
                .status_code = scada::StatusCode::Good,
                .references = {{.reference_type_id = NumericNode(81),
                                .forward = true,
                                .node_id = NumericNode(82)},
                               {.reference_type_id = NumericNode(83),
                                .forward = true,
                                .node_id = NumericNode(84)},
                               {.reference_type_id = NumericNode(85),
                                .forward = false,
                                .node_id = NumericNode(86)}}}};
          }));

  TlsBeastClient client;
  client.Connect("127.0.0.1", port(), "https://scada.local", "opcua+uajson");
  ExpectBrowsePagingRoundTrip(client);
  client.Close();
}

TEST_F(WebSocketServerTest, RejectsOriginOutsideAllowList) {
  StartServer();

  BeastClient client;
  EXPECT_THROW(
      client.Connect("127.0.0.1", port(), "https://evil.local", "opcua+uajson"),
      boost::system::system_error);
}

TEST_F(WebSocketServerTest, RejectsMissingRequiredSubprotocol) {
  StartServer();

  BeastClient client;
  EXPECT_THROW(client.Connect("127.0.0.1", port(), "https://scada.local", ""),
               boost::system::system_error);
}

TEST_F(WebSocketServerTest, RejectsOriginOutsideAllowListOverTls) {
  StartServer(transport::WebSocketServerTlsConfig{
      .certificate_chain_pem = kTestCertificatePem,
      .private_key_pem = kTestPrivateKeyPem,
  });

  TlsBeastClient client;
  EXPECT_THROW(
      client.Connect("127.0.0.1", port(), "https://evil.local", "opcua+uajson"),
      boost::system::system_error);
}

TEST_F(WebSocketServerTest,
       PublishDoesNotBlockCreateMonitoredItemsOnSameSocket) {
  StartServer();

  BeastClient client;
  client.Connect("127.0.0.1", port(), "https://scada.local", "opcua+uajson");

  const auto create_session = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 1, .body = CreateSessionRequest{}})));
  const auto created = std::get<CreateSessionResponse>(create_session.body);

  const auto activate_session = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 2,
                      .body = ActivateSessionRequest{
                          .session_id = created.session_id,
                          .authentication_token = created.authentication_token,
                          .user_name = scada::LocalizedText{u"operator"},
                          .password = scada::LocalizedText{u"secret"}}})));
  EXPECT_EQ(
      std::get<ActivateSessionResponse>(activate_session.body).status.code(),
      scada::StatusCode::Good);

  const auto create_subscription = *DecodeResponseMessage(boost::json::parse(
      client.Request({.request_handle = 3,
                      .body = CreateSubscriptionRequest{
                          .parameters = {.publishing_interval_ms = 500,
                                         .lifetime_count = 60,
                                         .max_keep_alive_count = 10,
                                         .publishing_enabled = true}}})));
  const auto subscription =
      std::get<CreateSubscriptionResponse>(create_subscription.body);

  client.Send({.request_handle = 4, .body = PublishRequest{}});
  client.Send(
      {.request_handle = 5,
       .body = CreateMonitoredItemsRequest{
           .subscription_id = subscription.subscription_id,
           .items_to_create = {
               {.item_to_monitor = {.node_id = NumericNode(11),
                                    .attribute_id = scada::AttributeId::Value},
                .requested_parameters = {.client_handle = 44,
                                         .sampling_interval_ms = 0,
                                         .queue_size = 1,
                                         .discard_oldest = true}}}}});

  const auto create_items = *DecodeResponseMessage(
      boost::json::parse(client.Read(std::chrono::milliseconds{200})));
  EXPECT_EQ(create_items.request_handle, 5u);
  EXPECT_EQ(
      std::get<CreateMonitoredItemsResponse>(create_items.body).status.code(),
      scada::StatusCode::Good);

  client.Close();
}

}  // namespace
}  // namespace opcua::ws
