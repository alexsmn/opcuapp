#include "session.h"

#include <opcuapp/assertions.h>
#include <opcuapp/requests.h>
#include <opcuapp/vector.h>

namespace opcua {
namespace server {

Session::Session(SessionContext&& context)
    : SessionContext{std::move(context)} {
}

Session::~Session() {
}

void Session::BeginInvoke(OpcUa_ActivateSessionRequest& request, const std::function<void(OpcUa_ActivateSessionResponse& response)>& callback) {
  ActivateSessionResponse response;
  callback(response);
}

void Session::BeginInvoke(OpcUa_ReadRequest& request, const std::function<void(OpcUa_ReadResponse& response)>& callback) {
  read_handler_(request, callback);
}

void Session::BeginInvoke(OpcUa_BrowseRequest& request, const std::function<void(OpcUa_BrowseResponse& response)>& callback) {
  browse_handler_(request, callback);
}

void Session::BeginInvoke(OpcUa_CreateSubscriptionRequest& request, const std::function<void(OpcUa_CreateSubscriptionResponse& response)>& callback) {
  auto subscription = CreateSubscription();

  CreateSubscriptionResponse response;
  response.SubscriptionId = subscription->id();
  response.RevisedLifetimeCount = request.RequestedLifetimeCount;
  response.RevisedMaxKeepAliveCount = request.RequestedMaxKeepAliveCount;
  response.RevisedPublishingInterval = request.RequestedPublishingInterval;
  callback(response);
}

void Session::BeginInvoke(OpcUa_CloseSessionRequest& request, const std::function<void(OpcUa_CloseSessionResponse& response)>& callback) {
  CloseSessionResponse response;
  callback(response);
}

void Session::BeginInvoke(OpcUa_PublishRequest& request, const std::function<void(OpcUa_PublishResponse& response)>& callback) {
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
    pending_request.callback = std::move(callback);

    pending_publish_requests_.emplace(std::move(pending_request));
  }

  OnPublishAvailable();
}

void Session::OnPublishAvailable() {
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

std::shared_ptr<Subscription> Session::GetSubscription(SubscriptionId id) {
  std::lock_guard<std::mutex> lock{mutex_};
  auto i = subscriptions_.find(id);
  return i != subscriptions_.end() ? i->second : nullptr;
}

std::shared_ptr<Subscription> Session::CreateSubscription() {
  std::lock_guard<std::mutex> lock{mutex_};

  const auto subscription_id = next_subscription_id_++;
  auto subscription = std::make_shared<Subscription>(SubscriptionContext{
      subscription_id,
      create_monitored_item_handler_,
      [this] { OnPublishAvailable(); },
  });

  subscriptions_.emplace(subscription_id, subscription);
  return subscription;
}

} // namespace server
} // namespace opcua