#include "opcua/discovery_client.h"

#include "opcua/net/net_executor_adapter.h"
#include "opcua/binary/client_connection.h"
#include "opcua/binary/client_secure_channel.h"
#include "opcua/binary/client_transport.h"
#include "opcua/endpoint_url.h"
#include "opcua/scada/node_id.h"
#include "transport/transport_factory.h"
#include "transport/transport_string.h"

#include <utility>
#include <variant>

namespace opcua {

using DiscoveryResult = StatusOr<std::vector<EndpointDescription>>;

DiscoveryClient::DiscoveryClient(AnyExecutor executor,
                                 transport::TransportFactory& transport_factory)
    : executor_{std::move(executor)}, transport_factory_{transport_factory} {}

Awaitable<DiscoveryResult> DiscoveryClient::GetEndpoints(
    std::string endpoint_url) {
  const auto parsed = ParseOpcTcpUrl(endpoint_url);
  if (!parsed.valid) {
    co_return DiscoveryResult{Status{StatusCode::Bad}};
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
    co_return DiscoveryResult{
        Status{StatusCode::Bad_Disconnected}};
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
    co_return DiscoveryResult{open_status};
  }

  const std::uint32_t request_id = connection.NextRequestId();
  const RequestMessage request{
      .request_handle = request_id,
      .body = RequestBody{GetEndpointsRequest{.endpoint_url = endpoint_url}},
  };
  auto send_status =
      co_await connection.SendRequest(request_id, request, NodeId{});
  if (send_status.bad()) {
    (void)co_await connection.Close();
    co_return DiscoveryResult{send_status};
  }

  auto frame = co_await connection.ReadResponse();
  (void)co_await connection.Close();
  if (!frame.ok()) {
    co_return DiscoveryResult{frame.status()};
  }

  auto& body = frame->message.body;
  if (auto* fault = std::get_if<ServiceFault>(&body)) {
    co_return DiscoveryResult{fault->status};
  }
  auto* response = std::get_if<GetEndpointsResponse>(&body);
  if (!response) {
    co_return DiscoveryResult{Status{StatusCode::Bad}};
  }
  if (response->status.bad()) {
    co_return DiscoveryResult{response->status};
  }
  co_return DiscoveryResult{std::move(response->endpoints)};
}

}  // namespace opcua
