#include "opcua/client/client_session.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/boost_log.h"
#include "opcua/client/client_subscription.h"
#include "opcua/client/discovery_client.h"
#include "opcua/client/endpoint_selection.h"
#include "opcua/client/endpoint_url.h"
#include "opcua/net/net_executor_adapter.h"
#include "opcua/session/session_types.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/types/co_result.h"
#include "opcua/types/read_value_id.h"
#include "opcua/types/standard_node_ids.h"
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

// The context's trace id, when it is a W3C traceparent suitable for the
// request-header AdditionalHeader carrier; empty otherwise (legacy UUID trace
// ids never reach the wire header).
std::string TraceParentFrom(const ServiceContext& context) {
  return IsW3CTraceParent(context.trace_id()) ? context.trace_id()
                                              : std::string{};
}

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
    const SessionSecuritySettings& settings) {
  SecurityPreference preference;
  switch (settings.mode) {
    case SessionSecuritySettings::Mode::None:
      preference.mode = SecurityPreference::Mode::None;
      break;
    case SessionSecuritySettings::Mode::Auto:
      preference.mode = SecurityPreference::Mode::Auto;
      break;
    case SessionSecuritySettings::Mode::SignAndEncrypt:
      preference.mode = SecurityPreference::Mode::SignAndEncrypt;
      break;
  }
  if (!settings.required_policy_uri.empty()) {
    preference.required_policy_uri = settings.required_policy_uri;
  }
  return preference;
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

ClientSession::~ClientSession() {
  // A consumer's subscription view can outlive this session, and closing one
  // reaches back into the session (its executor) to release the view's items.
  // Closing them here makes that a no-op, so a late view teardown cannot touch
  // a destroyed session.
  if (default_subscription_)
    default_subscription_->CloseAllViews(StatusCode::Bad_NoCommunication);
}

Awaitable<void> ClientSession::Connect(SessionConnectParams params) {
  (void)co_await ConnectStatus(std::move(params));
}

CoStatus ClientSession::ConnectStatus(SessionConnectParams params) {
  co_return co_await ConnectAsync(std::move(params));
}

Awaitable<void> ClientSession::Disconnect() {
  co_await DisconnectAsync();
}

Awaitable<void> ClientSession::Reconnect() {
  co_await ReconnectAsync();
}

CoStatus ClientSession::ConnectAsync(SessionConnectParams params) {
  // Use `connection_string` when populated, otherwise compose from `host`.
  std::string endpoint = params.connection_string.empty()
                             ? std::string{"opc.tcp://"} + params.host
                             : params.connection_string;
  const auto parsed = ParseEndpointUrl(endpoint);
  if (!parsed.valid) {
    co_return StatusCode::Bad;
  }

  // Optionally run GetEndpoints discovery to choose the endpoint security
  // before opening the working channel. With the default (Mode::None) this is
  // skipped entirely and the connection uses SecurityPolicy=None, preserving
  // the legacy behaviour and leaving non-secure callers unaffected.
  std::optional<binary::ClientSecureChannel::Security> security;
  ByteString expected_server_certificate;
  if (params.security.mode != SessionSecuritySettings::Mode::None) {
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
  ByteString client_certificate_der;
  ByteString client_nonce;
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
    co_return StatusCode::Bad_NoCommunication;
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
                             const ByteString& server_certificate,
                             const ByteString& server_nonce)
        -> StatusOr<ClientProtocolSession::ClientSignatureData> {
      std::vector<std::uint8_t> data;
      data.reserve(server_certificate.size() + server_nonce.size());
      const auto append = [&data](const ByteString& bytes) {
        data.insert(
            data.end(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
            reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size());
      };
      append(server_certificate);
      append(server_nonce);
      auto signature = channel->SignClientData(data);
      if (!signature.ok()) {
        return StatusOr<ClientProtocolSession::ClientSignatureData>{
            signature.status()};
      }
      return StatusOr<ClientProtocolSession::ClientSignatureData>{
          ClientProtocolSession::ClientSignatureData{
              .algorithm = std::move(signature->algorithm),
              .signature = std::move(signature->signature)}};
    };
  }

  const auto status = co_await session_->Create(
      Duration::FromMinutes(10), std::move(identity), std::move(credentials));
  if (status.bad()) {
    Reset();
    NotifyStateChanged(false, status);
    co_return status;
  }
  is_connected_ = true;
  // Best-effort: learn the server's namespace layout before reporting success.
  co_await ReadNamespaceArray();
  NotifyStateChanged(true, Status{StatusCode::Good});
  co_return StatusCode::Good;
}

Awaitable<void> ClientSession::ReadNamespaceArray() {
  std::vector<ReadValueId> inputs = {
      {.node_id = NodeId{id::Server_NamespaceArray},
       .attribute_id = AttributeId::Value}};
  auto result = co_await session_->Read(std::move(inputs));
  if (!result.ok() || result->empty()) {
    const Status status =
        result.ok() ? Status{StatusCode::Bad} : result.status();
    LOG_INFO(logger_) << "OPC UA NamespaceArray read failed"
                      << LOG_TAG("Status", ToString(status));
    co_return;
  }
  namespace_table_ = NamespaceTable::FromVariant(result->front().value);
  LOG_INFO(logger_) << "OPC UA NamespaceArray read"
                    << LOG_TAG("NamespaceCount", namespace_table_.size());
  co_return;
}

CoStatusOr<EndpointDescription> ClientSession::DiscoverAndSelectEndpoint(
    const std::string& endpoint_url,
    const SessionSecuritySettings& settings) {
  DiscoveryClient discovery{executor_, transport_factory_};
  auto endpoints = co_await discovery.GetEndpoints(endpoint_url);
  if (!endpoints.ok()) {
    co_return StatusOr<EndpointDescription>{endpoints.status()};
  }
  // A secured endpoint can only be opened with a client certificate to sign
  // and identify the session. Without one, advertise only None so Auto
  // selection falls back to the unsecured endpoint instead of choosing a
  // secured one that BuildChannelSecurity would then reject.
  const bool has_client_certificate =
      !settings.client_certificate_path.empty() &&
      !settings.client_private_key_path.empty();
  co_return SelectEndpoint(*endpoints, ToSecurityPreference(settings),
                           has_client_certificate
                               ? ClientCapabilities::Default()
                               : ClientCapabilities::NoneOnly());
}

// static
StatusOr<binary::ClientSecureChannel::Security>
ClientSession::BuildChannelSecurity(const EndpointDescription& endpoint,
                                    const SessionSecuritySettings& settings) {
  using Security = binary::ClientSecureChannel::Security;

  // An unsecured endpoint needs no certificates; the default Security
  // (SecurityPolicy=None) is what the single-argument channel uses anyway.
  if (endpoint.security_mode == MessageSecurityMode::None) {
    Security security;
    return StatusOr<Security>{std::move(security)};
  }

  // A secured endpoint requires the client's certificate/key (to sign and to
  // identify itself) and the server's certificate (to encrypt the OPN request
  // and verify the response). The server certificate comes from the discovered
  // endpoint as DER; the client material is loaded from the configured PEMs.
  auto client_cert_pem = ReadFile(settings.client_certificate_path);
  auto client_key_pem = ReadFile(settings.client_private_key_path);
  if (!client_cert_pem || !client_key_pem) {
    return StatusOr<Security>{Status{StatusCode::Bad}};
  }
  auto client_certificate =
      binary::crypto::LoadPemCertificate(*client_cert_pem);
  if (!client_certificate.ok()) {
    return StatusOr<Security>{client_certificate.status()};
  }
  auto client_private_key = binary::crypto::LoadPemPrivateKey(*client_key_pem);
  if (!client_private_key.ok()) {
    return StatusOr<Security>{client_private_key.status()};
  }
  const auto& server_der = endpoint.server_certificate;
  auto server_certificate =
      binary::crypto::LoadDerCertificate(std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t*>(server_der.data()),
          server_der.size()});
  if (!server_certificate.ok()) {
    return StatusOr<Security>{server_certificate.status()};
  }

  Security security;
  security.security_policy_uri = endpoint.security_policy_uri;
  security.security_mode = static_cast<binary::MessageSecurityMode>(
      static_cast<UInt32>(endpoint.security_mode));
  security.client_certificate = std::move(*client_certificate);
  security.client_private_key = std::move(*client_private_key);
  security.server_certificate = std::move(*server_certificate);
  return StatusOr<Security>{std::move(security)};
}

Awaitable<void> ClientSession::DisconnectAsync() {
  if (!session_) {
    co_return;
  }
  co_await session_->Close();
  Reset();
  NotifyStateChanged(false, Status{StatusCode::Good});
}

Awaitable<void> ClientSession::ReconnectAsync() {
  co_return;
}

bool ClientSession::IsConnected(Duration* ping_delay) const {
  if (ping_delay) {
    *ping_delay = {};
  }
  return is_connected_;
}

std::uint32_t ClientSession::GetAccessRights() const {
  // TODO: Carry the ActivateSession UserRights back from the server. Until
  // then the adapter reports full rights, matching the pre-existing behaviour.
  return ~std::uint32_t{0};
}

NodeId ClientSession::GetUserId() const {
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

SessionDebugger* ClientSession::GetSessionDebugger() {
  return nullptr;
}

StatusOr<std::unique_ptr<MonitoredItemSubscription>>
ClientSession::CreateSubscription(
    ServiceContext context,
    MonitoredItemSubscriptionOptions /*options*/) {
  if (!default_subscription_)
    default_subscription_ = ClientSubscription::Create(*this);
  // Stamp this caller's traceparent on the subscription's wire requests, so the
  // server's subscription spans continue the caller's trace. The session shares
  // one subscription across callers, so this is last-writer-wins; with tracing
  // off the traceparent is empty and nothing changes.
  default_subscription_->SetTraceParent(TraceParentFrom(context));
  // One server-side subscription, but a private view per caller: callers are
  // independent consumers and must not drain each other's notifications.
  return default_subscription_->CreateView();
}

CoStatusOr<std::vector<BrowseResult>> ClientSession::Browse(
    ServiceContext context,
    std::vector<BrowseDescription> inputs) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result =
      co_await session->Browse(std::move(inputs), TraceParentFrom(context));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<std::vector<BrowsePathResult>> ClientSession::TranslateBrowsePaths(
    std::vector<BrowsePath> inputs,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->TranslateBrowsePathsToNodeIds(
      std::move(inputs), std::move(trace_parent));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<std::vector<DataValue>> ClientSession::Read(
    ServiceContext context,
    std::shared_ptr<const std::vector<ReadValueId>> inputs) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto copy = *inputs;
  auto result =
      co_await session->Read(std::move(copy), TraceParentFrom(context));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<std::vector<StatusCode>> ClientSession::Write(
    ServiceContext context,
    std::shared_ptr<const std::vector<WriteValue>> inputs) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto copy = *inputs;
  auto result =
      co_await session->Write(std::move(copy), TraceParentFrom(context));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatus ClientSession::Call(NodeId node_id,
                             NodeId method_id,
                             std::vector<Variant> arguments,
                             NodeId /*user_id*/,
                             std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result =
      co_await session->Call(std::move(node_id), std::move(method_id),
                             std::move(arguments), std::move(trace_parent));
  if (result.ok()) {
    co_return result->status;
  }
  co_return result.status();
}

CoStatusOr<std::vector<AddNodesResult>> ClientSession::AddNodes(
    std::vector<AddNodesItem> inputs,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result =
      co_await session->AddNodes(std::move(inputs), std::move(trace_parent));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<std::vector<StatusCode>> ClientSession::DeleteNodes(
    std::vector<DeleteNodesItem> inputs,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result =
      co_await session->DeleteNodes(std::move(inputs), std::move(trace_parent));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<std::vector<StatusCode>> ClientSession::AddReferences(
    std::vector<AddReferencesItem> inputs,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->AddReferences(std::move(inputs),
                                                std::move(trace_parent));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<std::vector<StatusCode>> ClientSession::DeleteReferences(
    std::vector<DeleteReferencesItem> inputs,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  auto* session = session_.get();
  auto result = co_await session->DeleteReferences(std::move(inputs),
                                                   std::move(trace_parent));
  if (result.ok()) {
    co_return std::move(*result);
  }
  co_return result.status();
}

CoStatusOr<HistoryReadRawResult> ClientSession::HistoryReadRaw(
    HistoryReadRawDetails details,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  co_return co_await session_->HistoryReadRaw(std::move(details),
                                              std::move(trace_parent));
}

CoStatusOr<HistoryReadEventsResult> ClientSession::HistoryReadEvents(
    HistoryReadEventsDetails details,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  co_return co_await session_->HistoryReadEvents(std::move(details),
                                                 std::move(trace_parent));
}

CoStatusOr<std::vector<StatusCode>> ClientSession::HistoryUpdateData(
    UpdateDataDetails details,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  co_return co_await session_->HistoryUpdateData(std::move(details),
                                                 std::move(trace_parent));
}

CoStatusOr<std::vector<StatusCode>> ClientSession::HistoryUpdateEvent(
    UpdateEventDetails details,
    std::string trace_parent) {
  if (!is_connected_) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  assert(session_);
  co_return co_await session_->HistoryUpdateEvent(std::move(details),
                                                  std::move(trace_parent));
}

void ClientSession::Reset() {
  // Views outlive this session object (each consumer holds its own), so tell
  // them the subscription is gone. Otherwise a consumer waiting in ReadNext
  // waits forever instead of seeing the drop and re-arming on reconnect.
  if (default_subscription_) {
    default_subscription_->CloseAllViews(StatusCode::Bad_NoCommunication);
  }
  default_subscription_.reset();
  session_.reset();
  channel_.reset();
  connection_.reset();
  secure_channel_.reset();
  transport_.reset();
  is_connected_ = false;
}

void ClientSession::NotifyStateChanged(bool connected, Status status) {
  session_state_changed_(connected, status);
}

}  // namespace opcua
