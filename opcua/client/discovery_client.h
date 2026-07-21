#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/types/status_or.h"

#include <string>
#include <vector>

namespace transport {
class TransportFactory;
}  // namespace transport

namespace opcua {

// Performs OPC UA GetEndpoints discovery against a server. Discovery runs over
// a transient SecurityPolicy=None secure channel and needs no session: the
// spec (OPC UA Part 4 §5.4.4 / §7.6) lets a client read a server's endpoint
// list before opening a secured channel, which is exactly how the client
// learns which SecurityPolicy / MessageSecurityMode / UserTokenPolicy the
// server offers. The channel is opened, used for one GetEndpoints call, and
// closed; nothing here is retained between calls.
class DiscoveryClient {
 public:
  DiscoveryClient(AnyExecutor executor,
                  transport::TransportFactory& transport_factory);

  // Connects to `endpoint_url` ("opc.tcp://host[:port]"), opens a None channel,
  // calls GetEndpoints, and returns the server's endpoint descriptions. The
  // request's endpointUrl echoes `endpoint_url` so the server can return the
  // endpoints registered for that URL.
  [[nodiscard]] Awaitable<StatusOr<std::vector<EndpointDescription>>>
  GetEndpoints(std::string endpoint_url);

  // Connects to a Discovery Server at `endpoint_url`, opens a None channel, and
  // calls RegisterServer to register (is_online=true) or unregister
  // (is_online=false) `server`. OPC UA Part 4 §5.4.5 RegisterServer.
  [[nodiscard]] Awaitable<Status> RegisterServer(std::string endpoint_url,
                                                 RegisteredServer server);

  // Connects to a Discovery Server at `endpoint_url`, opens a None channel,
  // and calls FindServers, returning the known servers (the server's own
  // description plus RegisterServer registrations it holds). `server_uris`
  // optionally filters by application/product URI. OPC UA Part 4 §5.4.2
  // FindServers.
  [[nodiscard]] Awaitable<StatusOr<std::vector<ApplicationDescription>>>
  FindServers(std::string endpoint_url,
              std::vector<std::string> server_uris = {});

 private:
  // Opens a transient None channel to `endpoint_url`, sends the one request,
  // reads the one response, and closes. Returns the response body (fault
  // already converted to its status).
  [[nodiscard]] Awaitable<StatusOr<ResponseBody>> SendDiscoveryRequest(
      std::string endpoint_url,
      RequestBody request_body);

  const AnyExecutor executor_;
  transport::TransportFactory& transport_factory_;
};

}  // namespace opcua
