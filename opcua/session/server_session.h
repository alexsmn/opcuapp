#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/time/time.h"
#include "opcua/message.h"
#include "opcua/services/operation_limits.h"
#include "opcua/services/service_callbacks.h"
#include "opcua/services/service_message.h"
#include "opcua/session/server_subscription.h"

#include "opcua/services/service_context.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opcua {

struct ServerSessionContext {
  NodeId session_id;
  NodeId authentication_token;
  ServiceContext service_context;
  // Executor backing the LegacyMonitoredItemAdapter that each subscription
  // uses to create monitored items from the subscription-based service.
  AnyExecutor executor;
  ServiceCallbacks::CreateSubscriptionCallback create_subscription;
  OperationLimits operation_limits;
  std::function<base::Time()> now = &base::Time::Now;
};

class ServerSession : private ServerSessionContext {
 public:
  struct PublishPollResult {
    std::optional<PublishResponse> response;
    std::optional<base::TimeDelta> wait_for;
  };

  explicit ServerSession(ServerSessionContext&& context);

  const ServiceContext& GetServiceContext() const {
    return this->service_context;
  }

  CreateSubscriptionResponse CreateSubscription(
      const CreateSubscriptionRequest& request);
  CreateSubscriptionResponse CreateSubscriptionWithId(
      SubscriptionId subscription_id,
      const CreateSubscriptionRequest& request);
  ModifySubscriptionResponse ModifySubscription(
      const ModifySubscriptionRequest& request);
  SetPublishingModeResponse SetPublishingMode(
      const SetPublishingModeRequest& request);
  DeleteSubscriptionsResponse DeleteSubscriptions(
      const DeleteSubscriptionsRequest& request);
  TransferSubscriptionsResponse TransferSubscriptionsFrom(
      ServerSession& source,
      const TransferSubscriptionsRequest& request);

  CreateMonitoredItemsResponse CreateMonitoredItems(
      const CreateMonitoredItemsRequest& request);
  ModifyMonitoredItemsResponse ModifyMonitoredItems(
      const ModifyMonitoredItemsRequest& request);
  DeleteMonitoredItemsResponse DeleteMonitoredItems(
      const DeleteMonitoredItemsRequest& request);
  SetMonitoringModeResponse SetMonitoringMode(
      const SetMonitoringModeRequest& request);

  std::vector<StatusCode> AcknowledgePublishRequest(
      const PublishRequest& request);
  PublishPollResult PollPublish();
  PublishResponse Publish(const PublishRequest& request);
  RepublishResponse Republish(const RepublishRequest& request) const;
  BrowseResponse StoreBrowseResults(BrowseResponse response,
                                    size_t requested_max_references_per_node);
  BrowseNextResponse BrowseNext(const BrowseNextRequest& request);
  std::vector<SubscriptionId> GetSubscriptionIds() const;
  bool HasSubscription(SubscriptionId subscription_id) const;

 private:
  struct ByteStringHash {
    size_t operator()(const ByteString& value) const;
  };

  struct BrowseContinuationState {
    std::vector<ReferenceDescription> remaining_references;
  };

  using SubscriptionMap =
      std::unordered_map<SubscriptionId, std::unique_ptr<ServerSubscription>>;
  using BrowseContinuationMap =
      std::unordered_map<ByteString, BrowseContinuationState, ByteStringHash>;

  base::Time Now() const { return this->now(); }
  ServerSubscription* FindSubscription(SubscriptionId subscription_id);
  const ServerSubscription* FindSubscription(
      SubscriptionId subscription_id) const;
  void EraseSubscription(SubscriptionId subscription_id);
  void AdvancePublishCursorAfter(size_t index);
  size_t FindNextReadySubscription(base::Time now, bool require_pending) const;
  void RefreshNextSubscriptionId();
  ByteString MakeBrowseContinuationPoint();
  BrowseResult PageBrowseResult(BrowseResult result,
                                size_t requested_max_references_per_node);
  BrowseResult ResumeBrowseResult(const ByteString& continuation_point);

  SubscriptionMap subscriptions_;
  BrowseContinuationMap browse_continuations_;
  std::vector<SubscriptionId> publish_order_;
  SubscriptionId next_subscription_id_ = 1;
  size_t next_publish_index_ = 0;
  UInt32 next_browse_continuation_id_ = 1;
};

}  // namespace opcua
