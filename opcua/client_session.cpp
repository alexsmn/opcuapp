#include "opcua/client_session.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/boost_log.h"
#include "opcua/net/net_executor_adapter.h"
#include "opcua/binary/crypto.h"
#include "opcua/client_subscription.h"
#include "opcua/discovery_client.h"
#include "opcua/endpoint_selection.h"
#include "opcua/endpoint_url.h"
#include "opcua/scada/item_factory_subscription.h"
#include "opcua/scada/read_value_id.h"
#include "opcua/scada/session_service.h"
#include "opcua/scada/standard_node_ids.h"
#include "transport/transport_factory.h"
#include "transport/transport_string.h"

#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace opcua {

namespace {

BoostLogger logger_{LOG_NAME("OpcUaClientSession")};

// Reads an entire file into a string. Returns nullopt if the file cannot be
// opened (e.g. a missing certificate path).
std::optional<std::string> ReadFile(const std::string& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{stream},
                     std::istreambuf_iterator<char>{}};
}

// Maps the transport-neutral SessionSecuritySettings onto the OPC UA endpoint
// SecurityPreference used by SelectEndpoint.
SecurityPreference ToSecurityPreference(
    const scada::SessionSecuritySettings& settings) {
  SecurityPreference preference;
  switch (settings.mode) {
    case scada::SessionSecuritySettings::Mode::None:
      preference.mode = SecurityPreference::Mode::None;
      break;
    case scada::SessionSecuritySettings::Mode::Auto:
      preference.mode = SecurityPreference::Mode::Auto;
      break;
    case scada::SessionSecuritySettings::Mode::SignAndEncrypt:
      preference.mode = SecurityPreference::Mode::SignAndEncrypt;
      break;
  }
  if (!settings.required_policy_uri.empty()) {
    preference.required_policy_uri = settings.required_policy_uri;
  }
  return preference;
}

template <typename Handler>
void DispatchLegacyCallback(const AnyExecutor& executor,
                            std::weak_ptr<ClientSession> weak_self,
                            Handler handler) {
  CoSpawn(executor,
          [weak_self = std::move(weak_self),
           handler = std::move(handler)]() mutable -> Awaitable<void> {
            auto self = weak_self.lock();
            if (!self) {
              co_return;
            }
            co_await handler(*self);
          });
}

}  // namespace

// static
ClientSession::ParsedEndpoint ClientSession::ParseEndpointUrl(
    const std::string& url) {
  // Delegates to the shared parser so the session and DiscoveryClient agree on
  // how an "opc.tcp://" URL maps to a transport target.
  const ParsedEndpointUrl parsed = ParseOpcTcpUrl(url);
  return ParsedEndpoint{
      .host = parsed.host, .port = parsed.port, .valid = parsed.valid};
}

ClientSession::ClientSession(AnyExecutor executor,
                             transport::TransportFactory& transport_factory)
    : executor_{std::move(executor)},
      any_executor_{executor_},
      transport_factory_{transport_factory} {}

ClientSession::~ClientSession() = default;

Awaitable<void> ClientSession::Connect(scada::SessionConnectParams params) {
  (void)co_await ConnectStatus(std::move(params));
}

Awaitable<scada::Status> ClientSession::ConnectStatus(
    scada::SessionConnectParams params) {
  co_return co_await ConnectAsync(std::move(params));
}

Awaitable<void> ClientSession::Disconnect() {
  co_await DisconnectAsync();
}

Awaitable<void> ClientSession::Reconnect() {
  co_await ReconnectAsync();
}

Awaitable<scada::Status> ClientSession::ConnectAsync(
    scada::SessionConnectParams params) {
  // Use `connection_string` when populated, otherwise compose from `host`.
  std::string endpoint = params.connection_string.empty()
                             ? std::string{"opc.tcp://"} + params.host
                             : params.connection_string;
  const auto parsed = ParseEndpointUrl(endpoint);
  if (!parsed.valid) {
    co_return scada::StatusCode::Bad;
  }

  // Optionally run GetEndpoints discovery to choose the endpoint security
  // before opening the working channel. With the default (Mode::None) this is
  // skipped entirely and the connection uses SecurityPolicy=None, preserving
  // the legacy behaviour and leaving non-secure callers unaffected.
  std::optional<binary::ClientSecureChannel::Security> security;
  scada::ByteString expected_server_certificate;
  if (params.security.mode != scada::SessionSecuritySettings::Mode::None) {
    auto chosen = co_await DiscoverAndSelectEndpoint(endpoint, params.security);
    if (!chosen.ok()) {
      NotifyStateChanged(false, chosen.status());
      co_return chosen.status();
    }
    // Remember the endpoint's certificate so the session can confirm the server
    // presents the same one in CreateSession.
    expected_server_certificate = chosen->server_certificate;
    auto built = BuildChannelSecurity(*chosen, params.security);
    if (!built.ok()) {
      NotifyStateChanged(false, built.status());
      co_return built.status();
    }
    security = std::move(*built);
  }

  // Capture the client certificate (DER) and a fresh client nonce before the
  // Security is moved into the secure channel below. Both are sent in
  // CreateSession so the server can bind and verify the ActivateSession
  // signature.
  scada::ByteString client_certificate_der;
  scada::ByteString client_nonce;
  const bool secured = security.has_value();
  if (secured) {
    if (auto der = binary::crypto::CertificateDer(security->client_certificate);
        der.ok()) {
      client_certificate_der = std::move(*der);
    }
    if (auto nonce = binary::crypto::GenerateNonce(32); nonce.ok()) {
      client_nonce = std::move(*nonce);
    }
  }

  transport::TransportString ts;
  ts.SetProtocol(transport::TransportString::TCP);
  ts.SetActive(true);
  ts.SetParam(transport::TransportString::kParamHost, parsed.host);
  ts.SetParam(transport::TransportString::kParamPort, parsed.port);

  const transport::executor net_executor{executor_};
  auto transport_result = transport_factory_.CreateTransport(
      ts, net_executor, transport::log_source{});
  if (!transport_result.ok()) {
    co_return scada::StatusCode::Bad_Disconnected;
  }

  endpoint_url_ = std::move(endpoint);
  transport_ =
      std::make_unique<binary::ClientTransport>(binary::ClientTransportContext{
          .transport = std::move(*transport_result),
          .endpoint_url = endpoint_url_,
          .limits = {},
      });
  secure_channel_ =
      security ? std::make_unique<binary::ClientSecureChannel>(
                     *transport_, std::move(*security))
               : std::make_unique<binary::ClientSecureChannel>(*transport_);
  connection_ = std::make_unique<binary::ClientConnection>(
      binary::ClientConnection::Context{
          .transport = *transport_,
          .secure_channel = *secure_channel_,
      });
  channel_ = std::make_unique<ClientChannel>(ClientChannel::Context{
      .executor = any_executor_,
      .connection = *connection_,
  });
  session_ =
      std::make_unique<ClientProtocolSession>(ClientProtocolSession::Context{
          .connection = *connection_,
          .channel = *channel_,
      });

  ClientProtocolSession::Identity identity;
  if (!params.user_name.empty()) {
    identity.user_name = params.user_name;
    identity.password = params.password;
  }

  // For a secured channel, supply the credentials and a signer that produces
  // the ActivateSession signature from the secure channel's client key. The
  // channel outlives this Create() call (torn down later in Reset()).
  ClientProtocolSession::ClientCredentials credentials;
  if (secured) {
    credentials.certificate = std::move(client_certificate_der);
    credentials.nonce = std::move(client_nonce);
    credentials.expected_server_certificate =
        std::move(expected_server_certificate);
    credentials.signer = [channel = secure_channel_.get()](
                             const scada::ByteString& server_certificate,
                             const scada::ByteString& server_nonce)
        -> scada::StatusOr<ClientProtocolSession::ClientSignatureData> {
      std::vector<std::uint8_t> data;
      data.reserve(server_certificate.size() + server_nonce.size());
      const auto append = [&data](const scada::ByteString& bytes) {
        data.insert(
            data.end(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
            reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size());
      };
      append(server_certificate);
      append(server_nonce);
      auto signature = channel->SignClientData(data);
      if (!signature.ok()) {
        return scada::StatusOr<ClientProtocolSession::ClientSignatureData>{
            signature.status()};
      }
      return scada::StatusOr<ClientProtocolSession::ClientSignatureData>{
          ClientProtocolSession::ClientSignatureData{
              .algorithm = std::move(signature->algorithm),
              .signature = std::move(signature->signature)}};
    };
  }

  const auto status =
      co_await session_->Create(base::TimeDelta::FromMinutes(10),
                                std::move(identity), std::move(credentials));
  if (status.bad()) {
    Reset();
    NotifyStateChanged(false, status);
    co_return status;
  }
  is_connected_ = true;
  // Best-effort: learn the server's namespace layout before reporting success.
  co_await ReadNamespaceArray();
  NotifyStateChanged(true, scada::Status{scada::StatusCode::Good});
  co_return scada::StatusCode::Good;
}

Awaitable<void> ClientSession::ReadNamespaceArray() {
  std::vector<scada::ReadValueId> inputs = {
      {.node_id = scada::NodeId{scada::id::Server_NamespaceArray},
       .attribute_id = scada::AttributeId::Value}};
  auto result = co_await session_->Read(std::move(inputs));
  if (!result.ok() || result->empty()) {
    const scada::Status status =
        result.ok() ? scada::Status{scada::StatusCode::Bad} : result.status();
    LOG_INFO(logger_) << "OPC UA NamespaceArray read failed"
                      << LOG_TAG("Status", ToString(status));
    co_return;
  }
  namespace_table_ = NamespaceTable::FromVariant(result->front().value);
  LOG_INFO(logger_) << "OPC UA NamespaceArray read"
                    << LOG_TAG("NamespaceCount", namespace_table_.size());
  co_return;
}

Awaitable<scada::StatusOr<EndpointDescription>>
ClientSession::DiscoverAndSelectEndpoint(
    const std::string& endpoint_url,
    const scada::SessionSecuritySettings& settings) {
  DiscoveryClient discovery{executor_, transport_factory_};
  auto endpoints = co_await discovery.GetEndpoints(endpoint_url);
  if (!endpoints.ok()) {
    co_return scada::StatusOr<EndpointDescription>{endpoints.status()};
  }
  co_return SelectEndpoint(*endpoints, ToSecurityPreference(settings),
                           ClientCapabilities::Default());
}

// static
scada::StatusOr<binary::ClientSecureChannel::Security>
ClientSession::BuildChannelSecurity(
    const EndpointDescription& endpoint,
    const scada::SessionSecuritySettings& settings) {
  using Security = binary::ClientSecureChannel::Security;

  // An unsecured endpoint needs no certificates; the default Security
  // (SecurityPolicy=None) is what the single-argument channel uses anyway.
  if (endpoint.security_mode == MessageSecurityMode::None) {
    Security security;
    return scada::StatusOr<Security>{std::move(security)};
  }

  // A secured endpoint requires the client's certificate/key (to sign and to
  // identify itself) and the server's certificate (to encrypt the OPN request
  // and verify the response). The server certificate comes from the discovered
  // endpoint as DER; the client material is loaded from the configured PEMs.
  auto client_cert_pem = ReadFile(settings.client_certificate_path);
  auto client_key_pem = ReadFile(settings.client_private_key_path);
  if (!client_cert_pem || !client_key_pem) {
    return scada::StatusOr<Security>{scada::Status{scada::StatusCode::Bad}};
  }
  auto client_certificate =
      binary::crypto::LoadPemCertificate(*client_cert_pem);
  if (!client_certificate.ok()) {
    return scada::StatusOr<Security>{client_certificate.status()};
  }
  auto client_private_key = binary::crypto::LoadPemPrivateKey(*client_key_pem);
  if (!client_private_key.ok()) {
    return scada::StatusOr<Security>{client_private_key.status()};
  }
  const auto& server_der = endpoint.server_certificate;
  auto server_certificate =
      binary::crypto::LoadDerCertificate(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t*>(server_der.data()),
          server_der.size()});
  if (!server_certificate.ok()) {
    return scada::StatusOr<Security>{server_certificate.status()};
  }

  Security security;
  security.security_policy_uri = endpoint.security_policy_uri;
  security.security_mode = static_cast<binary::MessageSecurityMode>(
      static_cast<scada::UInt32>(endpoint.security_mode));
  security.client_certificate = std::move(*client_certificate);
  security.client_private_key = std::move(*client_private_key);
  security.server_certificate = std::move(*server_certificate);
  return scada::StatusOr<Security>{std::move(security)};
}

Awaitable<void> ClientSession::DisconnectAsync() {
  if (!session_) {
    co_return;
  }
  co_await session_->Close();
  Reset();
  NotifyStateChanged(false, scada::Status{scada::StatusCode::Good});
}

Awaitable<void> ClientSession::ReconnectAsync() {
  co_return;
}

bool ClientSession::IsConnected(base::TimeDelta* ping_delay) const {
  if (ping_delay) {
    *ping_delay = {};
  }
  return is_connected_;
}

bool ClientSession::HasPrivilege(scada::Privilege) const {
  return true;
}

scada::NodeId ClientSession::GetUserId() const {
  return {};
}

std::string ClientSession::GetHostName() const {
  return {};
}

boost::signals2::scoped_connection ClientSession::SubscribeSessionStateChanged(
    const SessionStateChangedCallback& callback) {
  return boost::signals2::scoped_connection{
      session_state_changed_.connect(callback)};
}

scada::SessionDebugger* ClientSession::GetSessionDebugger() {
  return nullptr;
}

scada::StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
ClientSession::CreateSubscription(
    scada::ServiceContext /*context*/,
    scada::MonitoredItemSubscriptionOptions options) {
  return scada::MakeItemFactorySubscription(
      [this](const scada::ReadValueId& read_value_id,
             const scada::MonitoringParameters& params) {
        assert(!read_value_id.node_id.is_null());
        return GetDefaultSubscription().CreateMonitoredItem(read_value_id,
                                                            params);
      },
      options);
}

Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>>
ClientSession::Browse(scada::ServiceContext /*context*/,
                      std::vector<scada::BrowseDescription> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->Browse(std::move(inputs));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::BrowsePathResult>>>
ClientSession::TranslateBrowsePaths(std::vector<scada::BrowsePath> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result =
      co_await session->TranslateBrowsePathsToNodeIds(std::move(inputs));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> ClientSession::Read(
    scada::ServiceContext /*context*/,
    std::shared_ptr<const std::vector<scada::ReadValueId>> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto copy = *inputs;
  auto result = co_await session->Read(std::move(copy));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>> ClientSession::Write(
    scada::ServiceContext /*context*/,
    std::shared_ptr<const std::vector<scada::WriteValue>> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto copy = *inputs;
  auto result = co_await session->Write(std::move(copy));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::Status> ClientSession::Call(
    scada::NodeId node_id,
    scada::NodeId method_id,
    std::vector<scada::Variant> arguments,
    scada::NodeId /*user_id*/) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->Call(std::move(node_id), std::move(method_id),
                                       std::move(arguments));
  if (result.ok()) {
    co_return result->status;
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::AddNodesResult>>>
ClientSession::AddNodes(std::vector<scada::AddNodesItem> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->AddNodes(std::move(inputs));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientSession::DeleteNodes(std::vector<scada::DeleteNodesItem> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->DeleteNodes(std::move(inputs));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientSession::AddReferences(std::vector<scada::AddReferencesItem> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->AddReferences(std::move(inputs));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
ClientSession::DeleteReferences(
    std::vector<scada::DeleteReferencesItem> inputs) {
  if (!is_connected_) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->DeleteReferences(std::move(inputs));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

ClientSubscription& ClientSession::GetDefaultSubscription() {
  if (!default_subscription_) {
    default_subscription_ = ClientSubscription::Create(*this);
  }
  return *default_subscription_;
}

void ClientSession::Reset() {
  default_subscription_.reset();
  session_.reset();
  channel_.reset();
  connection_.reset();
  secure_channel_.reset();
  transport_.reset();
  is_connected_ = false;
}

void ClientSession::NotifyStateChanged(bool connected, scada::Status status) {
  session_state_changed_(connected, status);
}

}  // namespace opcua
