#include "opcua/client/discovery_client.h"

#include "opcua/client/client_security.h"
#include "opcua/client/endpoint_selection.h"
#include "opcua/client/endpoint_url.h"
#include "opcua/net/net_executor_adapter.h"
#include "opcua/transport/binary/client_connection.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
#include "opcua/types/co_result.h"
#include "opcua/types/node_id.h"
#include "transport/transport_factory.h"
#include "transport/transport_string.h"

#include <utility>
#include <variant>

namespace opcua {

using DiscoveryResult = StatusOr<std::vector<EndpointDescription>>;

DiscoveryClient::DiscoveryClient(AnyExecutor executor,
                                 transport::TransportFactory& transport_factory)
    : executor_{std::move(executor)}, transport_factory_{transport_factory} {}

DiscoveryClient::DiscoveryClient(AnyExecutor executor,
                                 transport::TransportFactory& transport_factory,
                                 SessionSecuritySettings settings)
    : executor_{std::move(executor)},
      transport_factory_{transport_factory},
      settings_{std::move(settings)} {}

CoStatusOr<binary::ClientSecureChannel::Security>
DiscoveryClient::ResolveChannelSecurity(const std::string& endpoint_url) {
  using Result = StatusOr<binary::ClientSecureChannel::Security>;
  if (settings_.mode == SessionSecuritySettings::Mode::None) {
    // Default-initialized, not `Security{}`: the aggregate holds a
    // crypto::Certificate whose default constructor is explicit, which makes
    // brace-init ill-formed in a copy-initialization context.
    binary::ClientSecureChannel::Security unsecured;
    co_return Result{std::move(unsecured)};
  }
  // Bootstrap: learn the peer's endpoints (and certificate) over an unsecured
  // channel, then let BuildChannelSecurity authenticate that certificate
  // against the trust store before anything is encrypted to it.
  auto endpoints = co_await GetEndpoints(endpoint_url);
  if (!endpoints.ok()) {
    co_return Result{endpoints.status()};
  }
  auto chosen = SelectEndpoint(*endpoints, ToSecurityPreference(settings_),
                               CapabilitiesFor(settings_));
  if (!chosen.ok()) {
    co_return Result{chosen.status()};
  }
  co_return BuildChannelSecurity(*chosen, settings_);
}

CoStatusOr<ResponseBody> DiscoveryClient::SendDiscoveryRequest(
    std::string endpoint_url,
    RequestBody request_body,
    binary::ClientSecureChannel::Security security) {
  using Result = StatusOr<ResponseBody>;

  const auto parsed = ParseOpcTcpUrl(endpoint_url);
  if (!parsed.valid) {
    co_return Result{Status{StatusCode::Bad}};
  }

  transport::TransportString transport_string;
  transport_string.SetProtocol(transport::TransportString::TCP);
  transport_string.SetActive(true);
  transport_string.SetParam(transport::TransportString::kParamHost,
                            parsed.host);
  transport_string.SetParam(transport::TransportString::kParamPort,
                            parsed.port);

  const transport::executor net_executor{executor_};
  auto transport_result = transport_factory_.CreateTransport(
      transport_string, net_executor, transport::log_source{});
  if (!transport_result.ok()) {
    co_return Result{Status{StatusCode::Bad_NoCommunication}};
  }

  // The whole stack lives on this coroutine frame and is torn down on return.
  // Discovery never uses the background ClientChannel read loop: the single
  // request/response is driven inline through the connection so no detached
  // coroutine outlives these locals.
  binary::ClientTransport transport{binary::ClientTransportContext{
      .transport = std::move(*transport_result),
      .endpoint_url = endpoint_url,
      .limits = {},
  }};
  binary::ClientSecureChannel secure_channel{transport,
                                            std::move(security)};
  binary::ClientConnection connection{binary::ClientConnection::Context{
      .transport = transport,
      .secure_channel = secure_channel,
  }};

  auto open_status = co_await connection.Open();
  if (open_status.bad()) {
    co_return Result{open_status};
  }

  const std::uint32_t request_id = connection.NextRequestId();
  const RequestMessage request{
      .request_handle = request_id,
      .body = std::move(request_body),
  };
  auto send_status =
      co_await connection.SendRequest(request_id, request, NodeId{});
  if (send_status.bad()) {
    (void)co_await connection.Close();
    co_return Result{send_status};
  }

  auto frame = co_await connection.ReadResponse();
  (void)co_await connection.Close();
  if (!frame.ok()) {
    co_return Result{frame.status()};
  }

  auto& body = frame->message.body;
  if (auto* fault = std::get_if<ServiceFault>(&body)) {
    co_return Result{fault->status};
  }
  co_return Result{std::move(body)};
}

Awaitable<DiscoveryResult> DiscoveryClient::GetEndpoints(
    std::string endpoint_url) {
  // Deliberately unsecured, even when settings_ ask for security: this is the
  // bootstrap that discovers the certificate any secured channel would need.
  // Default-initialized rather than `Security{}` — see ResolveChannelSecurity.
  binary::ClientSecureChannel::Security unsecured;
  auto body = co_await SendDiscoveryRequest(
      endpoint_url,
      RequestBody{GetEndpointsRequest{.endpoint_url = endpoint_url}},
      std::move(unsecured));
  if (!body.ok()) {
    co_return DiscoveryResult{body.status()};
  }
  auto* response = std::get_if<GetEndpointsResponse>(&*body);
  if (!response) {
    co_return DiscoveryResult{Status{StatusCode::Bad}};
  }
  if (response->status.bad()) {
    co_return DiscoveryResult{response->status};
  }
  co_return DiscoveryResult{std::move(response->endpoints)};
}

CoStatus DiscoveryClient::RegisterServer(std::string endpoint_url,
                                         RegisteredServer server) {
  auto security = co_await ResolveChannelSecurity(endpoint_url);
  if (!security.ok()) {
    co_return security.status();
  }
  auto body = co_await SendDiscoveryRequest(
      std::move(endpoint_url),
      RequestBody{RegisterServerRequest{.server = std::move(server)}},
      std::move(*security));
  if (!body.ok()) {
    co_return body.status();
  }
  auto* response = std::get_if<RegisterServerResponse>(&*body);
  if (!response) {
    co_return Status{StatusCode::Bad};
  }
  co_return response->status;
}

CoStatus DiscoveryClient::RegisterServer2(
    std::string endpoint_url,
    RegisteredServer server,
    std::vector<std::string> server_capabilities) {
  RegisterServer2Request request{.server = std::move(server)};
  request.discovery_configuration.push_back(MdnsDiscoveryConfiguration{
      // mdnsServerName falls back to the server URI: this deployment does no
      // mDNS announcement, but the field is mandatory (Part 4 §7.8).
      .mdns_server_name = request.server.server_uri,
      .server_capabilities = std::move(server_capabilities)});
  auto security = co_await ResolveChannelSecurity(endpoint_url);
  if (!security.ok()) {
    co_return security.status();
  }
  auto body = co_await SendDiscoveryRequest(std::move(endpoint_url),
                                            RequestBody{std::move(request)},
                                            std::move(*security));
  if (!body.ok()) {
    co_return body.status();
  }
  auto* response = std::get_if<RegisterServer2Response>(&*body);
  if (!response) {
    co_return Status{StatusCode::Bad};
  }
  co_return response->status;
}

CoStatusOr<std::vector<ApplicationDescription>> DiscoveryClient::FindServers(
    std::string endpoint_url,
    std::vector<std::string> server_uris) {
  using Result = StatusOr<std::vector<ApplicationDescription>>;

  // No move of endpoint_url into the request: function-argument evaluation
  // order is unspecified, and the first argument needs the intact value.
  auto security = co_await ResolveChannelSecurity(endpoint_url);
  if (!security.ok()) {
    co_return Result{security.status()};
  }
  auto body = co_await SendDiscoveryRequest(
      endpoint_url,
      RequestBody{FindServersRequest{.endpoint_url = endpoint_url,
                                     .server_uris = std::move(server_uris)}},
      std::move(*security));
  if (!body.ok()) {
    co_return Result{body.status()};
  }
  auto* response = std::get_if<FindServersResponse>(&*body);
  if (!response) {
    co_return Result{Status{StatusCode::Bad}};
  }
  if (response->status.bad()) {
    co_return Result{response->status};
  }
  co_return Result{std::move(response->servers)};
}

}  // namespace opcua
