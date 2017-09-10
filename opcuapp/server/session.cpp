#include "session.h"

#include <opcuapp/requests.h>

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
  auto* subscription = CreateSubscription();

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
    if (auto* subscription = GetSubscription(acknowledgements[i].SubscriptionId)) {
      subscription->Acknowledge(acknowledgements[i].SequenceNumber);
      results[i] = OpcUa_Good;
    } else {
      results[i] = OpcUa_Bad;
    }
  }

  PublishResponse response;
  response.NoOfResults = results.size();
  response.Results = results.release();

  for (auto& p : subscriptions_) {
    if (p.second.Publish(response))
      break;
  }

  callback(response);
}

Subscription* Session::GetSubscription(SubscriptionId id) {
  auto i = subscriptions_.find(id);
  return i != subscriptions_.end() ? &i->second : nullptr;
}

Subscription* Session::CreateSubscription() {
  auto id = next_subscription_id_++;

  SubscriptionContext context{
      id,
      create_monitored_item_handler_,
  };

  auto i = subscriptions_.emplace(std::piecewise_construct,
      std::forward_as_tuple(id),
      std::forward_as_tuple(std::move(context)));
  return &i.first->second;
}

} // namespace server
} // namespace opcua