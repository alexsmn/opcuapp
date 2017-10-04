#pragma once

#include <algorithm>
#include <mutex>
#include <opcuapp/assertions.h>
#include <opcuapp/node_id.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/server/subscription.h>
#include <opcuapp/timer.h>
#include <opcuapp/vector.h>

namespace opcua {
namespace server {

template<class Timer>
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
  using Subscription = BasicSubscription<Timer>;

  explicit Session(SessionContext&& context);
  ~Session();

  const NodeId& id() const { return id_; }
  const String& name() const { return name_; }
  const NodeId& authentication_token() const { return authentication_token_; }

  std::shared_ptr<Subscription> GetSubscription(opcua::SubscriptionId id);

  using PublishCallback = std::function<void(PublishResponse& response)>;

  template<class ActivateSessionResponseHandler>
  void BeginInvoke(OpcUa_ActivateSessionRequest& request, ActivateSessionResponseHandler&& response_handler);

  template<class CloseSessionResponseHandler>
  void BeginInvoke(OpcUa_CloseSessionRequest& request, CloseSessionResponseHandler&& response_handler);

  template<class ReadResponseHandler>
  void BeginInvoke(OpcUa_ReadRequest& request, ReadResponseHandler&& response_handler);

  template<class BrowseResponseHandler>
  void BeginInvoke(OpcUa_BrowseRequest& request, BrowseResponseHandler&& response_handler);

  template<class CreateSubscriptionResponseHandler>
  void BeginInvoke(OpcUa_CreateSubscriptionRequest& request, CreateSubscriptionResponseHandler&& response_handler);

  template<class PublishResponseHandler>
  void BeginInvoke(OpcUa_PublishRequest& request, PublishResponseHandler&& response_handler);

 private:
  std::shared_ptr<Subscription> CreateSubscription(OpcUa_CreateSubscriptionRequest& request);

  void Publish();

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

inline Session::Session(SessionContext&& context)
    : SessionContext{std::move(context)} {
}

inline Session::~Session() {
}

template<class ActivateSessionResponseHandler>
inline void Session::BeginInvoke(OpcUa_ActivateSessionRequest& request, ActivateSessionResponseHandler&& response_handler) {
  ActivateSessionResponse response;
  response_handler(std::move(response));
}

template<class ReadResponseHandler>
inline void Session::BeginInvoke(OpcUa_ReadRequest& request, ReadResponseHandler&& response_handler) {
  read_handler_(request, std::forward<ReadResponseHandler>(response_handler));
}

template<class BrowseResponseHandler>
inline void Session::BeginInvoke(OpcUa_BrowseRequest& request, BrowseResponseHandler&& response_handler) {
  browse_handler_(request, std::forward<BrowseResponseHandler>(response_handler));
}

template<class CreateSubscriptionResponseHandler>
inline void Session::BeginInvoke(OpcUa_CreateSubscriptionRequest& request, CreateSubscriptionResponseHandler&& response_handler) {
  request.RequestedMaxKeepAliveCount = std::max<UInt32>(request.RequestedMaxKeepAliveCount, 1);

  auto subscription = CreateSubscription(request);

  CreateSubscriptionResponse response;
  response.SubscriptionId = subscription->id();
  response.RevisedLifetimeCount = request.RequestedLifetimeCount;
  response.RevisedMaxKeepAliveCount = request.RequestedMaxKeepAliveCount;
  response.RevisedPublishingInterval = request.RequestedPublishingInterval;
  response_handler(response);
}

template<class CloseSessionResponseHandler>
inline void Session::BeginInvoke(OpcUa_CloseSessionRequest& request, CloseSessionResponseHandler&& response_handler) {
  CloseSessionResponse response;
  response_handler(response);
}

template<class PublishResponseHandler>
inline void Session::BeginInvoke(OpcUa_PublishRequest& request, PublishResponseHandler&& response_handler) {
  Span<OpcUa_SubscriptionAcknowledgement> acknowledgements{
      request.SubscriptionAcknowledgements,
      static_cast<size_t>(request.NoOfSubscriptionAcknowledgements)
  };

  Vector<OpcUa_StatusCode> results(acknowledgements.size());

  for (size_t i = 0; i < acknowledgements.size(); ++i) {
    if (auto subscription = GetSubscription(acknowledgements[i].SubscriptionId)) {
      subscription->Acknowledge(acknowledgements[i].SequenceNumber);
      results[i] = OpcUa_Good;
    } else {
      results[i] = OpcUa_Bad;
    }
  }

  {
    std::lock_guard<std::mutex> lock{mutex_};

    PendingPublishRequest pending_request;
    pending_request.response.NoOfResults = results.size();
    pending_request.response.Results = results.release();
    pending_request.callback = std::forward<PublishResponseHandler>(response_handler);

    pending_publish_requests_.emplace(std::move(pending_request));
  }

  Publish();
}

inline void Session::Publish() {
  std::unique_lock<std::mutex> lock{mutex_};

  if (pending_publish_requests_.empty())
    return;

  auto& first_request_ref = pending_publish_requests_.front();
  for (auto& p : subscriptions_) {
    if (p.second->Publish(first_request_ref.response)) {
      assert(IsValid(first_request_ref.response.NotificationMessage));
      auto first_request = std::move(first_request_ref);
      assert(IsValid(first_request.response.NotificationMessage));
      pending_publish_requests_.pop();
      lock.unlock();
      first_request.callback(first_request.response);
      return;
    }
  }
}

inline std::shared_ptr<Session::Subscription> Session::GetSubscription(SubscriptionId id) {
  std::lock_guard<std::mutex> lock{mutex_};
  auto i = subscriptions_.find(id);
  return i != subscriptions_.end() ? i->second : nullptr;
}

inline std::shared_ptr<Session::Subscription> Session::CreateSubscription(OpcUa_CreateSubscriptionRequest& request) {
  std::lock_guard<std::mutex> lock{mutex_};

  const auto subscription_id = next_subscription_id_++;
  auto subscription = std::make_shared<Subscription>(SubscriptionContext{
      subscription_id,
      create_monitored_item_handler_,
      [this] { Publish(); },
      request.RequestedPublishingInterval,
      request.RequestedLifetimeCount,
      request.RequestedMaxKeepAliveCount,
      request.MaxNotificationsPerPublish != 0 ?
          static_cast<size_t>(request.MaxNotificationsPerPublish) :
          std::numeric_limits<size_t>::max(),
      request.PublishingEnabled != OpcUa_False,
      request.Priority,
  });

  subscriptions_.emplace(subscription_id, subscription);
  return subscription;
}

} // namespace server
} // namespace opcua