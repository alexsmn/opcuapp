#pragma once

#include "opcua/base/any_executor.h"

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/server/service_handler.h"
#include "opcua/services/operation_limits.h"
#include "opcua/services/service_callbacks.h"
#include "opcua/session/server_session.h"
#include "opcua/session/server_session_manager.h"
#include "opcua/types/date_time.h"

#include <optional>
#include <unordered_map>

namespace opcua {

struct ConnectionState {
  std::optional<NodeId> authentication_token;
  bool closed = false;
  // SecureChannel binding for this connection. Set by the binary transport
  // when the channel negotiated a secure policy: `secure_channel` is true and
  // `client_certificate` holds the client application instance certificate
  // (DER) presented during OpenSecureChannel. Both stay default under
  // SecurityPolicy=None and for the WS/TLS transport.
  bool secure_channel = false;
  ByteString client_certificate;
  // Remote network peer of this connection ("address:port"), captured by the
  // transport at accept time. Empty when the transport has no network peer.
  // Carried into session and per-request logs (the OTel `client.address`
  // equivalent) so records can be correlated to the originating client.
  std::string peer;
};

// Context of a RegisterServer/RegisterServer2 request. The security part lets
// the handler enforce that only trusted callers register discovery entries
// (OPC UA Part 4 §5.4.5; Part 2 §4 security objectives): `channel_secure` is
// true when the request arrived on a Sign/SignAndEncrypt SecureChannel;
// `client_certificate` holds the caller's application instance certificate
// (DER), if presented. `server_capabilities` carries the union of the
// ServerCapabilityIdentifiers from a RegisterServer2 request's
// discoveryConfiguration (OPC UA Part 4 §5.4.6, Part 12 Annex D; empty for
// plain RegisterServer).
struct RegisterServerContext {
  bool channel_secure = false;
  ByteString client_certificate;
  std::vector<std::string> server_capabilities;
};

struct ServerRuntimeContext {
  AnyExecutor executor;
  ServerSessionManager& session_manager;
  ServiceCallbacks callbacks;
  std::vector<EndpointDescription> endpoints;
  OperationLimits operation_limits;
  std::function<DateTime()> now = &DateTime::Now;
  // Optional override for delayed task scheduling. Defaults to
  // boost::asio::steady_timer-based posting when null.
  std::function<void(Duration, std::function<void()>)> post_delayed_task;
  // Optional RegisterServer handler (the aggregating proxy registers
  // downstreams here). When null the server rejects RegisterServer (it is not a
  // discovery server). OPC UA Part 4 §5.4.5 RegisterServer.
  std::function<Status(const RegisteredServer&, const RegisterServerContext&)>
      register_server;
  // Optional snapshot of the servers currently registered via RegisterServer.
  // When set, FindServers also returns these registrations (synthesized
  // ApplicationDescriptions), so clients can discover other site tiers
  // through this server — the discovery-server role of OPC UA Part 4 §5.4.2
  // FindServers. The server's own endpoints win on application_uri collision.
  std::function<std::vector<RegisteredServer>()> registered_servers;
};

class ServerRuntime {
 public:
  explicit ServerRuntime(ServerRuntimeContext&& context);
  ~ServerRuntime();

  // `trace_parent` is the W3C traceparent extracted from the request header
  // (RequestHeader.additionalHeader); when non-empty it overrides the session
  // context's trace id for this request, so service callbacks see the
  // caller's trace. Empty (the default, and what transports without header
  // propagation pass) keeps the session context untouched.
  [[nodiscard]] Awaitable<ResponseBody> Handle(ConnectionState& connection,
                                               RequestBody request,
                                               std::string trace_parent = {});
  void Detach(ConnectionState& connection);

 private:
  using SessionMap = std::unordered_map<NodeId, std::shared_ptr<ServerSession>>;

  [[nodiscard]] ServerSession* FindSession(
      const NodeId& authentication_token) const;
  [[nodiscard]] ServerSession* FindAttachedSession(
      const ConnectionState& connection) const;
  void ForgetSession(const NodeId& authentication_token);
  void RemoveSessionSubscriptions(const NodeId& authentication_token);
  [[nodiscard]] Awaitable<ResponseBody> HandleActivateSession(
      ConnectionState& connection,
      ActivateSessionRequest request);
  [[nodiscard]] ResponseBody HandleFindServers(
      const FindServersRequest& request) const;
  [[nodiscard]] ResponseBody HandleGetEndpoints(
      const GetEndpointsRequest& request) const;
  [[nodiscard]] ResponseBody HandleRegisterServer(
      const ConnectionState& connection,
      const RegisterServerRequest& request) const;
  [[nodiscard]] ResponseBody HandleRegisterServer2(
      const ConnectionState& connection,
      const RegisterServer2Request& request) const;
  [[nodiscard]] Awaitable<ServiceResponse> HandleServiceRequest(
      const ServerSession& session,
      ServiceRequest request,
      const std::string& trace_parent) const;
  [[nodiscard]] Awaitable<void> Delay(Duration delay) const;

  SessionMap sessions_;
  std::unordered_map<SubscriptionId, NodeId> subscription_owners_;
  SubscriptionId next_subscription_id_ = 1;

  AnyExecutor executor_;
  ServerSessionManager& session_manager_;
  ServiceCallbacks callbacks_;
  std::vector<EndpointDescription> endpoints_;
  OperationLimits operation_limits_;
  std::function<DateTime()> now_;
  std::function<void(Duration, std::function<void()>)> post_delayed_task_;
  std::function<Status(const RegisteredServer&, const RegisterServerContext&)>
      register_server_;
  std::function<std::vector<RegisteredServer>()> registered_servers_;
};

}  // namespace opcua
