#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/any_executor_dispatch.h"
#include "opcua/base/awaitable.h"
#include "opcua/binary/client_connection.h"
#include "opcua/binary/client_secure_channel.h"
#include "opcua/binary/client_transport.h"
#include "opcua/client_channel.h"
#include "opcua/client_protocol_session.h"
#include "opcua/message.h"
#include "opcua/namespace_table.h"
#include "opcua/scada/attribute_service.h"
#include "opcua/scada/coroutine_services.h"
#include "opcua/scada/method_service.h"
#include "opcua/scada/monitored_item_service.h"
#include "opcua/scada/node_management_service.h"
#include "opcua/scada/session_service.h"
#include "opcua/scada/view_service.h"

#include <boost/signals2/signal.hpp>
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

// Qt client's adapter onto the in-repo OPC UA Binary client (see
// common/opcua/binary/*). Implements the scada::* service interfaces over the
// coroutine-native client stack underneath.
class ClientSession final : public std::enable_shared_from_this<ClientSession>,
                            public SessionService,
                            public ViewService,
                            public AttributeService,
                            public MethodService,
                            public NodeManagementService,
                            public scada::MonitoredItemService {
 public:
  ClientSession(AnyExecutor executor,
                transport::TransportFactory& transport_factory);
  ~ClientSession() override;

  // SessionService
  Awaitable<void> Connect(SessionConnectParams params) override;
  Awaitable<Status> ConnectStatus(
      SessionConnectParams params) override;
  Awaitable<void> Disconnect() override;
  Awaitable<void> Reconnect() override;
  bool IsConnected(base::TimeDelta* ping_delay = nullptr) const override;
  bool HasPrivilege(Privilege privilege) const override;
  bool IsScada() const override { return false; }
  NodeId GetUserId() const override;
  std::string GetHostName() const override;
  boost::signals2::scoped_connection SubscribeSessionStateChanged(
      const SessionStateChangedCallback& callback) override;
  SessionDebugger* GetSessionDebugger() override;

  [[nodiscard]] Awaitable<Status> ConnectAsync(
      SessionConnectParams params);
  [[nodiscard]] Awaitable<void> DisconnectAsync();
  [[nodiscard]] Awaitable<void> ReconnectAsync();

  // scada::MonitoredItemService
  StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
  CreateSubscription(ServiceContext context,
                     scada::MonitoredItemSubscriptionOptions options) override;

  // ViewService
  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowseResult>>>
  Browse(ServiceContext context,
         std::vector<BrowseDescription> inputs) override;
  [[nodiscard]] Awaitable<StatusOr<std::vector<BrowsePathResult>>>
  TranslateBrowsePaths(std::vector<BrowsePath> inputs) override;

  // AttributeService
  [[nodiscard]] Awaitable<StatusOr<std::vector<DataValue>>> Read(
      ServiceContext context,
      std::shared_ptr<const std::vector<ReadValueId>> inputs) override;
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  Write(ServiceContext context,
        std::shared_ptr<const std::vector<WriteValue>> inputs) override;

  // MethodService
  [[nodiscard]] Awaitable<Status> Call(
      NodeId node_id,
      NodeId method_id,
      std::vector<Variant> arguments,
      NodeId user_id) override;

  // NodeManagementService
  [[nodiscard]] Awaitable<StatusOr<std::vector<AddNodesResult>>>
  AddNodes(std::vector<AddNodesItem> inputs) override;
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  DeleteNodes(std::vector<DeleteNodesItem> inputs) override;
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  AddReferences(std::vector<AddReferencesItem> inputs) override;
  [[nodiscard]] Awaitable<StatusOr<std::vector<StatusCode>>>
  DeleteReferences(std::vector<DeleteReferencesItem> inputs) override;

  // The server's namespace table, read from Server_NamespaceArray after the
  // session is activated. Empty until a successful connect (and if the server
  // does not publish it). Lets callers translate namespace URIs to the indices
  // this server assigns.
  [[nodiscard]] const NamespaceTable& namespace_table() const {
    return namespace_table_;
  }

  // Access for ClientSubscription / MonitoredItem.
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

  ClientSubscription& GetDefaultSubscription();

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

  boost::signals2::signal<void(bool, const Status&)>
      session_state_changed_;
};

}  // namespace opcua
