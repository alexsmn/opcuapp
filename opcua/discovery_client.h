#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/scada/status_or.h"

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
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<EndpointDescription>>>
  GetEndpoints(std::string endpoint_url);

 private:
  const AnyExecutor executor_;
  transport::TransportFactory& transport_factory_;
};

}  // namespace opcua
