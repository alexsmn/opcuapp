#pragma once

#include "opcuapp/node_id.h"
#include "opcuapp/server/handlers.h"
#include "opcuapp/server/subscription.h"

namespace opcua {
namespace server {

class Subscription;

struct SessionContext {
  const NodeId id_;
  const String name_;
  const NodeId authentication_token_;
  const ReadHandler read_handler_;
  const BrowseHandler browse_handler_;
};

class Session : private SessionContext {
 public:
  explicit Session(SessionContext&& context) : SessionContext{std::move(context)} {}

  const NodeId& id() const { return id_; }
  const String& name() const { return name_; }
  const NodeId& authentication_token() const { return authentication_token_; }

  Subscription* GetSubscription(opcua::SubscriptionId id);

  void BeginInvoke(OpcUa_ActivateSessionRequest& request, const std::function<void(OpcUa_ActivateSessionResponse& response)>& callback);
  void BeginInvoke(OpcUa_CloseSessionRequest& request, const std::function<void(OpcUa_CloseSessionResponse& response)>& callback);
  void BeginInvoke(OpcUa_ReadRequest& request, const std::function<void(OpcUa_ReadResponse& response)>& callback);
  void BeginInvoke(OpcUa_BrowseRequest& request, const std::function<void(OpcUa_BrowseResponse& response)>& callback);
  void BeginInvoke(OpcUa_CreateSubscriptionRequest& request, const std::function<void(OpcUa_CreateSubscriptionResponse& response)>& callback);
  void BeginInvoke(OpcUa_PublishRequest& request, const std::function<void(OpcUa_PublishResponse& response)>& callback);

 private:
  Subscription* CreateSubscription();

  std::map<opcua::SubscriptionId, Subscription> subscriptions_;
  opcua::SubscriptionId next_subscription_id_ = 1;
};

} // namespace server
} // namespace opcua