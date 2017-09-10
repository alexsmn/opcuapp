#pragma once

#include <mutex>
#include <opcuapp/node_id.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/server/subscription.h>

namespace opcua {
namespace server {

class Subscription;

struct SessionContext {
  const NodeId id_;
  const String name_;
  const NodeId authentication_token_;
  const ReadHandler read_handler_;
  const BrowseHandler browse_handler_;
  const CreateMonitoredItemHandler create_monitored_item_handler_;
};

class Session : private SessionContext {
 public:
  explicit Session(SessionContext&& context);
  ~Session();

  const NodeId& id() const { return id_; }
  const String& name() const { return name_; }
  const NodeId& authentication_token() const { return authentication_token_; }

  std::shared_ptr<Subscription> GetSubscription(opcua::SubscriptionId id);

  using PublishCallback = std::function<void(OpcUa_PublishResponse& response)>;

  void BeginInvoke(OpcUa_ActivateSessionRequest& request, const std::function<void(OpcUa_ActivateSessionResponse& response)>& callback);
  void BeginInvoke(OpcUa_CloseSessionRequest& request, const std::function<void(OpcUa_CloseSessionResponse& response)>& callback);
  void BeginInvoke(OpcUa_ReadRequest& request, const std::function<void(OpcUa_ReadResponse& response)>& callback);
  void BeginInvoke(OpcUa_BrowseRequest& request, const std::function<void(OpcUa_BrowseResponse& response)>& callback);
  void BeginInvoke(OpcUa_CreateSubscriptionRequest& request, const std::function<void(OpcUa_CreateSubscriptionResponse& response)>& callback);
  void BeginInvoke(OpcUa_PublishRequest& request, const PublishCallback& callback);

 private:
  std::shared_ptr<Subscription> CreateSubscription();

  void OnPublishAvailable();

  std::mutex mutex_;

  std::map<opcua::SubscriptionId, std::shared_ptr<Subscription>> subscriptions_;
  opcua::SubscriptionId next_subscription_id_ = 1;

  struct PendingPublishRequest {
    PublishRequest request;
    PublishResponse response;
    PublishCallback callback;
  };

  std::queue<PendingPublishRequest> pending_publish_requests_;
};

} // namespace server
} // namespace opcua