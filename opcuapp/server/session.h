#pragma once

#include <opcuapp/assertions.h>
#include <opcuapp/node_id.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/server/subscription.h>
#include <opcuapp/timer.h>
#include <opcuapp/vector.h>
#include <algorithm>
#include <list>
#include <mutex>

namespace opcua {
namespace server {

template <class Timer>
class Subscription;

struct SessionContext {
  const NodeId id_;
  const String name_;
  const NodeId authentication_token_;
  const SessionHandlers handlers_;
};

class Session : public std::enable_shared_from_this<Session>,
                private SessionContext {
 public:
  using Subscription = BasicSubscription<Timer>;

  explicit Session(SessionContext&& context);
  ~Session();

  const NodeId& id() const { return id_; }
  const String& name() const { return name_; }
  const NodeId& authentication_token() const { return authentication_token_; }

  std::shared_ptr<Subscription> GetSubscription(opcua::SubscriptionId id);

  using PublishCallback = std::function<void(PublishResponse&& response)>;

  template <class ActivateSessionResponseHandler>
  void BeginInvoke(OpcUa_ActivateSessionRequest& request,
                   ActivateSessionResponseHandler&& response_handler);

  template <class CloseSessionResponseHandler>
  void BeginInvoke(OpcUa_CloseSessionRequest& request,
                   CloseSessionResponseHandler&& response_handler);

  template <class ReadResponseHandler>
  void BeginInvoke(OpcUa_ReadRequest& request,
                   ReadResponseHandler&& response_handler);

  template <class BrowseResponseHandler>
  void BeginInvoke(OpcUa_BrowseRequest& request,
                   BrowseResponseHandler&& response_handler);

  template <class TranslateBrowsePathsToNodeIdsResponseHandler>
  void BeginInvoke(
      OpcUa_TranslateBrowsePathsToNodeIdsRequest& request,
      TranslateBrowsePathsToNodeIdsResponseHandler&& response_handler);

  template <class CreateSubscriptionResponseHandler>
  void BeginInvoke(OpcUa_CreateSubscriptionRequest& request,
                   CreateSubscriptionResponseHandler&& response_handler);

  template <class DeleteSubscriptionsResponseHandler>
  void BeginInvoke(OpcUa_DeleteSubscriptionsRequest& request,
                   DeleteSubscriptionsResponseHandler&& response_handler);

  template <class PublishResponseHandler>
  void BeginInvoke(OpcUa_PublishRequest& request,
                   PublishResponseHandler&& response_handler);

  void Close();

 private:
  struct PendingPublishRequest {
    std::chrono::steady_clock::time_point start_time;
    RequestHeader header;
    PublishResponse response;
    PublishCallback callback;
  };

  std::shared_ptr<Subscription> CreateSubscription(
      OpcUa_CreateSubscriptionRequest& request);

  void Publish();

  void DeleteSubscription(SubscriptionId subscription_id);

  void CheckPendingPublishRequestTimeouts();

  static bool IsTimedOut(const PendingPublishRequest& request);

  std::mutex mutex_;

  using Subscriptions = std::map<SubscriptionId, std::shared_ptr<Subscription>>;
  Subscriptions subscriptions_;

  SubscriptionId next_subscription_id_ = 1;

  std::list<PendingPublishRequest> pending_publish_requests_;

  Timer pending_publish_requests_timer_;

  bool closed_ = false;
};

inline Session::Session(SessionContext&& context)
    : SessionContext{std::move(context)} {
  pending_publish_requests_timer_.set_interval(1000);
  pending_publish_requests_timer_.Start(
      [this] { CheckPendingPublishRequestTimeouts(); });
}

inline Session::~Session() {}

template <class ActivateSessionResponseHandler>
inline void Session::BeginInvoke(
    OpcUa_ActivateSessionRequest& request,
    ActivateSessionResponseHandler&& response_handler) {
  // TODO: |closed_|

  ActivateSessionResponse response;
  response_handler(std::move(response));
}

template <class ReadResponseHandler>
inline void Session::BeginInvoke(OpcUa_ReadRequest& request,
                                 ReadResponseHandler&& response_handler) {
  // TODO: |closed_|

  handlers_.read_handler_(request,
                          std::forward<ReadResponseHandler>(response_handler));
}

template <class BrowseResponseHandler>
inline void Session::BeginInvoke(OpcUa_BrowseRequest& request,
                                 BrowseResponseHandler&& response_handler) {
  // TODO: |closed_|

  handlers_.browse_handler_(
      request, std::forward<BrowseResponseHandler>(response_handler));
}

template <class TranslateBrowsePathsToNodeIdsResponseHandler>
inline void Session::BeginInvoke(
    OpcUa_TranslateBrowsePathsToNodeIdsRequest& request,
    TranslateBrowsePathsToNodeIdsResponseHandler&& response_handler) {
  // TODO: |closed_|

  handlers_.translate_browse_paths_to_node_ids_handler_(
      request, std::forward<TranslateBrowsePathsToNodeIdsResponseHandler>(
                   response_handler));
}

template <class CreateSubscriptionResponseHandler>
inline void Session::BeginInvoke(
    OpcUa_CreateSubscriptionRequest& request,
    CreateSubscriptionResponseHandler&& response_handler) {
  // TODO: |closed_|

  request.RequestedMaxKeepAliveCount =
      std::max<UInt32>(request.RequestedMaxKeepAliveCount, 1);

  auto subscription = CreateSubscription(request);

  CreateSubscriptionResponse response;
  response.SubscriptionId = subscription->id();
  response.RevisedLifetimeCount = request.RequestedLifetimeCount;
  response.RevisedMaxKeepAliveCount = request.RequestedMaxKeepAliveCount;
  response.RevisedPublishingInterval = request.RequestedPublishingInterval;
  response_handler(std::move(response));
}

template <class DeleteSubscriptionsResponseHandler>
inline void Session::BeginInvoke(
    OpcUa_DeleteSubscriptionsRequest& request,
    DeleteSubscriptionsResponseHandler&& response_handler) {
  std::vector<std::shared_ptr<Subscription>> subscriptions;
  subscriptions.reserve(request.NoOfSubscriptionIds);

  Vector<OpcUa_StatusCode> results(request.NoOfSubscriptionIds);

  {
    std::lock_guard<std::mutex> lock{mutex_};

    for (OpcUa_Int32 i = 0; i < request.NoOfSubscriptionIds; ++i) {
      auto subscription_id = request.SubscriptionIds[i];
      auto& result = results[i];

      auto p = subscriptions_.find(subscription_id);
      if (p != subscriptions_.end()) {
        subscriptions.emplace_back(p->second);
        subscriptions_.erase(p);
        result = OpcUa_Good;
      } else {
        result = OpcUa_BadSubscriptionIdInvalid;
      }
    }
  }

  for (auto& subscription : subscriptions)
    subscription->Close();

  DeleteSubscriptionsResponse response;
  response.NoOfResults = results.size();
  response.Results = results.release();
  response_handler(std::move(response));
}

template <class CloseSessionResponseHandler>
inline void Session::BeginInvoke(
    OpcUa_CloseSessionRequest& request,
    CloseSessionResponseHandler&& response_handler) {
  // TODO: |closed_|

  CloseSessionResponse response;
  response_handler(std::move(response));
}

template <class PublishResponseHandler>
inline void Session::BeginInvoke(OpcUa_PublishRequest& request,
                                 PublishResponseHandler&& response_handler) {
  Span<OpcUa_SubscriptionAcknowledgement> acknowledgements{
      request.SubscriptionAcknowledgements,
      static_cast<size_t>(request.NoOfSubscriptionAcknowledgements)};

  Vector<OpcUa_StatusCode> results(acknowledgements.size());

  for (size_t i = 0; i < acknowledgements.size(); ++i) {
    results[i] = OpcUa_Bad;
    auto subscription = GetSubscription(acknowledgements[i].SubscriptionId);
    if (subscription &&
        subscription->Acknowledge(acknowledgements[i].SequenceNumber))
      results[i] = OpcUa_Good;
  }

  {
    std::lock_guard<std::mutex> lock{mutex_};

    if (closed_) {
      // TODO:
      return;
    }

    PendingPublishRequest pending_request;
    pending_request.start_time = std::chrono::steady_clock::now();
    pending_request.header = std::move(request.RequestHeader);
    pending_request.response.NoOfResults = results.size();
    pending_request.response.Results = results.release();
    pending_request.callback =
        std::forward<PublishResponseHandler>(response_handler);

    pending_publish_requests_.emplace_back(std::move(pending_request));
  }

  Publish();
}

inline void Session::Publish() {
  std::vector<PendingPublishRequest> completed_requests;

  {
    std::unique_lock<std::mutex> lock{mutex_};

    if (closed_)
      return;

    while (!pending_publish_requests_.empty()) {
      bool request_completed = false;
      auto& request = pending_publish_requests_.front();

      for (auto& p : subscriptions_) {
        if (p.second->Publish(request.response)) {
          assert(IsValid(request.response.NotificationMessage));
          completed_requests.emplace_back(std::move(request));
          pending_publish_requests_.pop_front();
          request_completed = true;
          break;
        }
      }

      if (!request_completed)
        break;
    }
  }

  for (auto& request : completed_requests)
    request.callback(std::move(request.response));
}

inline std::shared_ptr<Session::Subscription> Session::GetSubscription(
    SubscriptionId id) {
  std::lock_guard<std::mutex> lock{mutex_};
  auto i = subscriptions_.find(id);
  return i != subscriptions_.end() ? i->second : nullptr;
}

inline std::shared_ptr<Session::Subscription> Session::CreateSubscription(
    OpcUa_CreateSubscriptionRequest& request) {
  std::lock_guard<std::mutex> lock{mutex_};

  assert(!closed_);

  auto ref = shared_from_this();

  const auto subscription_id = next_subscription_id_++;
  auto subscription = Subscription::Create(SubscriptionContext{
      subscription_id,
      request.RequestedPublishingInterval,
      request.RequestedLifetimeCount,
      request.RequestedMaxKeepAliveCount,
      request.MaxNotificationsPerPublish != 0
          ? static_cast<size_t>(request.MaxNotificationsPerPublish)
          : std::numeric_limits<size_t>::max(),
      request.PublishingEnabled != OpcUa_False,
      request.Priority,
      handlers_.create_monitored_item_handler_,
      [ref] { ref->Publish(); },
      [ref, subscription_id] { ref->DeleteSubscription(subscription_id); },
  });

  subscriptions_.emplace(subscription_id, subscription);
  return subscription;
}

inline void Session::DeleteSubscription(SubscriptionId subscription_id) {
  std::lock_guard<std::mutex> lock{mutex_};
  subscriptions_.erase(subscription_id);
}

inline void Session::CheckPendingPublishRequestTimeouts() {
  std::vector<PendingPublishRequest> timed_out_requests;

  {
    std::lock_guard<std::mutex> lock{mutex_};
    for (auto i = pending_publish_requests_.begin();
         i != pending_publish_requests_.end();) {
      auto& request = *i;
      if (IsTimedOut(request)) {
        timed_out_requests.emplace_back(std::move(request));
        i = pending_publish_requests_.erase(i);
      } else {
        ++i;
      }
    }
  }

  for (auto& request : timed_out_requests) {
    request.response.ResponseHeader.ServiceResult = OpcUa_BadTimeout;
    request.callback(std::move(request.response));
  }
}

inline void Session::Close() {
  Subscriptions subscriptions;

  {
    std::lock_guard<std::mutex> lock{mutex_};

    if (closed_)
      return;

    closed_ = true;
    subscriptions = std::move(subscriptions_);
  }

  for (auto& p : subscriptions)
    p.second->Close();
}

// statuc
inline bool Session::IsTimedOut(const PendingPublishRequest& request) {
  auto timeout_ms = request.header.TimeoutHint;
  return timeout_ms != 0 &&
         std::chrono::steady_clock::now() - request.start_time >=
             std::chrono::milliseconds{timeout_ms};
}

}  // namespace server
}  // namespace opcua
