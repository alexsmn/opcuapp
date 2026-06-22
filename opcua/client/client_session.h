#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/any_executor_dispatch.h"
#include "opcua/base/awaitable.h"
#include "opcua/client/client_channel.h"
#include "opcua/client/client_protocol_session.h"
#include "opcua/client/namespace_table.h"
#include "opcua/message.h"
#include "opcua/monitored/monitored_item.h"
#include "opcua/services/attribute_types.h"
#include "opcua/services/method_types.h"
#include "opcua/services/node_management_types.h"
#include "opcua/services/view_types.h"
#include "opcua/session/session_types.h"
#include "opcua/transport/binary/client_connection.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"

#include <boost/signals2/signal.hpp>
#include <functional>
#include <memory>
#include <tuple>

namespace transport {
class TransportFactory;
}  // namespace transport

namespace opcua {
struct SessionSecuritySettings;
}  // namespace opcua

namespace opcua {

class ClientSubscription;
class SessionDebugger;

// Qt client's adapter onto the in-repo OPC UA Binary client. Provides concrete
// OPC UA client operations over the coroutine-native client stack underneath.
class ClientSession final : public std::enable_shared_from_this<ClientSession> {
 public:
  using SessionStateChangedCallback =
      std::function<void(bool connected, const Status& status)>;

  ClientSession(AnyExecutor executor,
                transport::TransportFactory& transport_factory);
  ~ClientSession();

  Awaitable<void> Connect(SessionConnectParams params);
  Awaitable<Status> ConnectStatus(SessionConnectParams params);
  Awaitable<void> Disconnect();
  Awaitable<void> Reconnect();
  bool IsConnected(base::TimeDelta* ping_delay = nullptr) const;
  bool HasPrivilege(Privilege privilege) const;
  bool IsScada() const { return false; }
  NodeId GetUserId() const;
  std::string GetHostName() const;
  boost::signals2::scoped_connection SubscribeSessionStateChanged(
      const SessionStateChangedCallback& callback);
  SessionDebugger* GetSessionDebugger();

  [[nodiscard]] Awaitable<Status> ConnectAsync(SessionConnectParams params);
  [[nodiscard]] Awaitable<void> DisconnectAsync();
  [[nodiscard]] Awaitable<void> ReconnectAsync();

  StatusOr<std::unique_ptr<MonitoredItemSubscription>> CreateSubscription(
      ServiceContext context,
      MonitoredItemSubscriptionOptions options);

  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowseResult>>> Browse(
      ServiceContext context,
      std::vector<BrowseDescription> inputs);
  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowsePathResult>>>
  TranslateBrowsePaths(std::vector<BrowsePath> inputs);

  [[nodiscard]] Awaitable<StatusOr<std::vector<DataValue>>> Read(
      ServiceContext context,
      std::shared_ptr<const std::vector<ReadValueId>> inputs);
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>> Write(
      ServiceContext context,
      std::shared_ptr<const std::vector<WriteValue>> inputs);

  [[nodiscard]] Awaitable<Status> Call(NodeId node_id,
                                       NodeId method_id,
                                       std::vector<Variant> arguments,
                                       NodeId user_id);

  [[nodiscard]] Awaitable<StatusOr<std::vector<AddNodesResult>>> AddNodes(
      std::vector<AddNodesItem> inputs);
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>> DeleteNodes(
      std::vector<DeleteNodesItem> inputs);
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>> AddReferences(
      std::vector<AddReferencesItem> inputs);
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>> DeleteReferences(
      std::vector<DeleteReferencesItem> inputs);

  // The server's namespace table, read from Server_NamespaceArray after the
  // session is activated. Empty until a successful connect (and if the server
  // does not publish it). Lets callers translate namespace URIs to the indices
  // this server assigns.
  [[nodiscard]] const NamespaceTable& namespace_table() const {
    return namespace_table_;
  }

  // Access for ClientSubscription.
  [[nodiscard]] ClientChannel& channel() { return *channel_; }
  [[nodiscard]] const AnyExecutor& any_executor() const {
    return any_executor_;
  }
  [[nodiscard]] bool is_connected() const { return is_connected_; }

 private:
  // Internal teardown on error or Disconnect.
  void Reset();

  // Parses "opc.tcp://host:port" into a transport string acceptable to
  // TransportFactory (TCP;Active;Host=...;Port=...).
  struct ParsedEndpoint {
    std::string host;
    int port = 4840;
    bool valid = false;
  };
  static ParsedEndpoint ParseEndpointUrl(const std::string& url);

  // Runs GetEndpoints discovery against `endpoint_url` over a transient
  // SecurityPolicy=None channel and selects the endpoint that best matches
  // `settings` and this client's capabilities. Used only when the caller
  // requests a non-default (discovery-driven) security mode.
  [[nodiscard]] Awaitable<StatusOr<EndpointDescription>>
  DiscoverAndSelectEndpoint(const std::string& endpoint_url,
                            const SessionSecuritySettings& settings);

  // Builds the secure-channel Security for a chosen endpoint: a None Security
  // for SecurityPolicy=None, otherwise the client certificate/key from
  // `settings` plus the server certificate carried by the endpoint.
  [[nodiscard]] static StatusOr<binary::ClientSecureChannel::Security>
  BuildChannelSecurity(const EndpointDescription& endpoint,
                       const SessionSecuritySettings& settings);

  // Reads Server_NamespaceArray into `namespace_table_`. Best-effort: a failure
  // is logged and leaves the table empty without aborting the connection.
  [[nodiscard]] Awaitable<void> ReadNamespaceArray();

  void NotifyStateChanged(bool connected, Status status);

  const AnyExecutor executor_;
  const AnyExecutor any_executor_;
  transport::TransportFactory& transport_factory_;

  // Entire client stack is lazily constructed on Connect() and torn down on
  // Disconnect() / error.
  std::unique_ptr<binary::ClientTransport> transport_;
  std::unique_ptr<binary::ClientSecureChannel> secure_channel_;
  std::unique_ptr<binary::ClientConnection> connection_;
  std::unique_ptr<ClientChannel> channel_;
  std::unique_ptr<ClientProtocolSession> session_;

  bool is_connected_ = false;
  std::string endpoint_url_;
  NamespaceTable namespace_table_;

  // Lazily created on first CreateMonitoredItem.
  std::shared_ptr<ClientSubscription> default_subscription_;

  boost::signals2::signal<void(bool, const Status&)> session_state_changed_;
};

}  // namespace opcua
