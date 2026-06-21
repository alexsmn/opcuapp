#pragma once

#include "opcua/base/any_executor.h"

#include "opcua/base/awaitable.h"
#include "opcua/base/time/time.h"
#include "opcua/message.h"
#include "opcua/operation_limits.h"
#include "opcua/server_session.h"
#include "opcua/server_session_manager.h"
#include "opcua/service_handler.h"
#include "opcua/scada/data_services.h"

#include <optional>
#include <unordered_map>

namespace opcua {
class AttributeService;
class HistoryService;
class MethodService;
class NodeManagementService;
class ViewService;
}  // namespace opcua

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
};

struct ServerRuntimeContext {
  AnyExecutor executor;
  ServerSessionManager& session_manager;
  scada::MonitoredItemService& monitored_item_service;
  AttributeService& attribute_service;
  ViewService& view_service;
  HistoryService& history_service;
  MethodService& method_service;
  NodeManagementService& node_management_service;
  std::vector<EndpointDescription> endpoints;
  OperationLimits operation_limits;
  std::function<base::Time()> now = &base::Time::Now;
  // Optional override for delayed task scheduling. Defaults to
  // boost::asio::steady_timer-based posting when null.
  std::function<void(base::TimeDelta, std::function<void()>)> post_delayed_task;
};

struct DataServicesServerRuntimeContext {
  AnyExecutor executor;
  ServerSessionManager& session_manager;
  DataServices data_services;
  std::vector<EndpointDescription> endpoints;
  OperationLimits operation_limits;
  std::function<base::Time()> now = &base::Time::Now;
  // Optional override for delayed task scheduling. Defaults to
  // boost::asio::steady_timer-based posting when null.
  std::function<void(base::TimeDelta, std::function<void()>)> post_delayed_task;
};

class ServerRuntime {
 public:
  explicit ServerRuntime(ServerRuntimeContext&& context);
  explicit ServerRuntime(DataServicesServerRuntimeContext&& context);
  ~ServerRuntime();

  [[nodiscard]] Awaitable<ResponseBody> Handle(ConnectionState& connection,
                                               RequestBody request);
  void Detach(ConnectionState& connection);

 private:
  using SessionMap =
      std::unordered_map<NodeId, std::shared_ptr<ServerSession>>;

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
  [[nodiscard]] Awaitable<ServiceResponse> HandleServiceRequest(
      const ServerSession& session,
      ServiceRequest request) const;
  [[nodiscard]] Awaitable<void> Delay(base::TimeDelta delay) const;

  SessionMap sessions_;
  std::unordered_map<SubscriptionId, NodeId> subscription_owners_;
  SubscriptionId next_subscription_id_ = 1;

  DataServices data_services_;
  AnyExecutor executor_;
  ServerSessionManager& session_manager_;
  scada::MonitoredItemService& monitored_item_service_;
  AttributeService& attribute_service_;
  ViewService& view_service_;
  HistoryService& history_service_;
  MethodService& method_service_;
  NodeManagementService& node_management_service_;
  std::vector<EndpointDescription> endpoints_;
  OperationLimits operation_limits_;
  std::function<base::Time()> now_;
  std::function<void(base::TimeDelta, std::function<void()>)>
      post_delayed_task_;
};

}  // namespace opcua
