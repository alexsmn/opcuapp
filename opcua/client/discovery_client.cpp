#include "opcua/client/discovery_client.h"

#include "opcua/client/endpoint_url.h"
#include "opcua/net/net_executor_adapter.h"
#include "opcua/transport/binary/client_connection.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
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

Awaitable<StatusOr<ResponseBody>> DiscoveryClient::SendDiscoveryRequest(
    std::string endpoint_url,
    RequestBody request_body) {
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
  binary::ClientSecureChannel secure_channel{transport};
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
  auto body = co_await SendDiscoveryRequest(
      endpoint_url,
      RequestBody{GetEndpointsRequest{.endpoint_url = endpoint_url}});
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

Awaitable<Status> DiscoveryClient::RegisterServer(std::string endpoint_url,
                                                  RegisteredServer server) {
  auto body = co_await SendDiscoveryRequest(
      std::move(endpoint_url),
      RequestBody{RegisterServerRequest{.server = std::move(server)}});
  if (!body.ok()) {
    co_return body.status();
  }
  auto* response = std::get_if<RegisterServerResponse>(&*body);
  if (!response) {
    co_return Status{StatusCode::Bad};
  }
  co_return response->status;
}

Awaitable<StatusOr<std::vector<ApplicationDescription>>>
DiscoveryClient::FindServers(std::string endpoint_url,
                             std::vector<std::string> server_uris) {
  using Result = StatusOr<std::vector<ApplicationDescription>>;

  // No move of endpoint_url into the request: function-argument evaluation
  // order is unspecified, and the first argument needs the intact value.
  auto body = co_await SendDiscoveryRequest(
      endpoint_url,
      RequestBody{FindServersRequest{.endpoint_url = endpoint_url,
                                     .server_uris = std::move(server_uris)}});
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
