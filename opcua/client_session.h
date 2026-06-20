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

namespace opcua::scada {
struct SessionSecuritySettings;
}  // namespace opcua::scada

namespace opcua {

class ClientSubscription;

// Qt client's adapter onto the in-repo OPC UA Binary client (see
// common/opcua/binary/*). Implements the scada::* service interfaces over the
// coroutine-native client stack underneath.
class ClientSession final : public std::enable_shared_from_this<ClientSession>,
                            public scada::SessionService,
                            public scada::ViewService,
                            public scada::AttributeService,
                            public scada::MethodService,
                            public scada::NodeManagementService,
                            public scada::MonitoredItemService {
 public:
  ClientSession(AnyExecutor executor,
                transport::TransportFactory& transport_factory);
  ~ClientSession() override;

  // scada::SessionService
  Awaitable<void> Connect(scada::SessionConnectParams params) override;
  Awaitable<scada::Status> ConnectStatus(
      scada::SessionConnectParams params) override;
  Awaitable<void> Disconnect() override;
  Awaitable<void> Reconnect() override;
  bool IsConnected(base::TimeDelta* ping_delay = nullptr) const override;
  bool HasPrivilege(scada::Privilege privilege) const override;
  bool IsScada() const override { return false; }
  scada::NodeId GetUserId() const override;
  std::string GetHostName() const override;
  boost::signals2::scoped_connection SubscribeSessionStateChanged(
      const SessionStateChangedCallback& callback) override;
  scada::SessionDebugger* GetSessionDebugger() override;

  [[nodiscard]] Awaitable<scada::Status> ConnectAsync(
      scada::SessionConnectParams params);
  [[nodiscard]] Awaitable<void> DisconnectAsync();
  [[nodiscard]] Awaitable<void> ReconnectAsync();

  // scada::MonitoredItemService
  scada::StatusOr<std::unique_ptr<scada::MonitoredItemSubscription>>
  CreateSubscription(scada::ServiceContext context,
                     scada::MonitoredItemSubscriptionOptions options) override;

  // scada::ViewService
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::BrowseResult>>>
  Browse(scada::ServiceContext context,
         std::vector<scada::BrowseDescription> inputs) override;
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::BrowsePathResult>>>
  TranslateBrowsePaths(std::vector<scada::BrowsePath> inputs) override;

  // scada::AttributeService
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::DataValue>>> Read(
      scada::ServiceContext context,
      std::shared_ptr<const std::vector<scada::ReadValueId>> inputs) override;
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  Write(scada::ServiceContext context,
        std::shared_ptr<const std::vector<scada::WriteValue>> inputs) override;

  // scada::MethodService
  [[nodiscard]] Awaitable<scada::Status> Call(
      scada::NodeId node_id,
      scada::NodeId method_id,
      std::vector<scada::Variant> arguments,
      scada::NodeId user_id) override;

  // scada::NodeManagementService
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::AddNodesResult>>>
  AddNodes(std::vector<scada::AddNodesItem> inputs) override;
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  DeleteNodes(std::vector<scada::DeleteNodesItem> inputs) override;
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  AddReferences(std::vector<scada::AddReferencesItem> inputs) override;
  [[nodiscard]] Awaitable<scada::StatusOr<std::vector<scada::StatusCode>>>
  DeleteReferences(std::vector<scada::DeleteReferencesItem> inputs) override;

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
  [[nodiscard]] Awaitable<scada::StatusOr<EndpointDescription>>
  DiscoverAndSelectEndpoint(const std::string& endpoint_url,
                            const scada::SessionSecuritySettings& settings);

  // Builds the secure-channel Security for a chosen endpoint: a None Security
  // for SecurityPolicy=None, otherwise the client certificate/key from
  // `settings` plus the server certificate carried by the endpoint.
  [[nodiscard]] static scada::StatusOr<binary::ClientSecureChannel::Security>
  BuildChannelSecurity(const EndpointDescription& endpoint,
                       const scada::SessionSecuritySettings& settings);

  // Reads Server_NamespaceArray into `namespace_table_`. Best-effort: a failure
  // is logged and leaves the table empty without aborting the connection.
  [[nodiscard]] Awaitable<void> ReadNamespaceArray();

  void NotifyStateChanged(bool connected, scada::Status status);

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

  boost::signals2::signal<void(bool, const scada::Status&)>
      session_state_changed_;
};

}  // namespace opcua
