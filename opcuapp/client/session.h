#pragma once

#include "opcuapp/client/async_request.h"
#include "opcuapp/client/channel.h"
#include "opcuapp/requests.h"
#include "opcuapp/structs.h"
#include "opcuapp/status_code.h"
#include "opcuapp/types.h"
#include "opcuapp/node_id.h"

#include <memory>
#include <map>
#include <mutex>
#include <vector>

namespace opcua {
namespace client {

class Subscription;

struct SessionInfo {
  NodeId session_id;
  NodeId authentication_token;
  Double revised_timeout;
  ByteString server_nonce;
  ByteString server_certificate;
};

class Session {
 public:
  explicit Session(Channel& channel) : channel_{channel} {}

  Channel& channel() { return channel_; }

  using CreateCallback = std::function<void(StatusCode status_code)>;
  void Create(const OpcUa_CreateSessionRequest& request, const CreateCallback& callback);

  using ActivateCallback = std::function<void(StatusCode status_code)>;
  void Activate(const ActivateCallback& callback);

  using BrowseCallback = std::function<void(StatusCode status_code, Span<OpcUa_BrowseResult> results)>;
  void Browse(Span<const OpcUa_BrowseDescription> descriptions, const BrowseCallback& callback);

  using ReadCallback = std::function<void(StatusCode status_code, Span<OpcUa_DataValue> results)>;
  void Read(Span<const OpcUa_ReadValueId> read_ids, const ReadCallback& callback);

  using NotificationHandler = std::function<void(Span<OpcUa_ExtensionObject> notifications)>;
  void StartPublishing(SubscriptionId subscription_id, NotificationHandler handler);
  void StopPublishing(SubscriptionId subscription_id);

  void Reset();

 private:
  void SetSessionInfo(OpcUa_CreateSessionResponse& response);

  void InitRequestHeader(OpcUa_RequestHeader& header) const;

  void Publish();
  void OnPublishResponse(StatusCode status_code, SubscriptionId subscription_id,
      Span<SequenceNumber> available_sequence_numbers, bool more_notifications,
      OpcUa_NotificationMessage& notification_message, Span<OpcUa_StatusCode> results);

  void OnActivated(ByteString server_nonce);
  void OnError(StatusCode status_code);

  Channel& channel_;

  mutable std::mutex mutex_;
  SessionInfo info_;
  std::map<SubscriptionId, NotificationHandler> subscriptions_;
  std::vector<OpcUa_SubscriptionAcknowledgement> acknowledgements_;
  std::vector<OpcUa_SubscriptionAcknowledgement> sent_acknowledgements_;
  bool publishing_ = false;

  friend class Subscription;
};

inline void Session::Create(const OpcUa_CreateSessionRequest& request, const CreateCallback& callback) {
  using Request = AsyncRequest<CreateSessionResponse>;
  auto async_request = std::make_unique<Request>([this, callback](CreateSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code) {
      std::lock_guard<std::mutex> lock{mutex_};
      info_.session_id.swap(response.SessionId);
      info_.authentication_token.swap(response.AuthenticationToken);
      info_.revised_timeout = response.RevisedSessionTimeout;
      info_.server_nonce.swap(response.ServerNonce);
      info_.server_certificate.swap(response.ServerCertificate);
    }
    callback(status_code);
  });

  StatusCode status_code = OpcUa_ClientApi_BeginCreateSession(
      channel_.handle(),
      &request.RequestHeader,
      &request.ClientDescription,
      &request.ServerUri,
      &request.EndpointUrl,
      &request.SessionName,
      &request.ClientNonce,
      &request.ClientCertificate,
      request.RequestedSessionTimeout,
      request.MaxResponseMessageSize,
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    callback(status_code);
}

inline void Session::Activate(const ActivateCallback& callback) {
  /*{
    std::lock_guard<std::mutex> lock{mutex_};
    acknowledgements_.insert(acknowledgements_.begin(), sent_acknowledgements_.begin(),
                                                        sent_acknowledgements_.end());
    sent_acknowledgements_.clear();
    publishing_ = false;
  }*/

  ActivateSessionRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<ActivateSessionResponse>;
  auto async_request = std::make_unique<Request>([this, callback](ActivateSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code)
      OnActivated(std::move(response.ServerNonce));
    callback(status_code);
  });

  StatusCode status_code = OpcUa_ClientApi_BeginActivateSession(
      channel_.handle(),
      &request.RequestHeader,
      &request.ClientSignature,
      request.NoOfClientSoftwareCertificates,
      request.ClientSoftwareCertificates,
      request.NoOfLocaleIds,
      request.LocaleIds,
      &request.UserIdentityToken,
      &request.UserTokenSignature,
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    callback(status_code);
}

inline void Session::Browse(Span<const OpcUa_BrowseDescription> descriptions, const BrowseCallback& callback) {
  BrowseRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<BrowseResponse>;
  auto async_request = std::make_unique<Request>([callback](BrowseResponse& response) {
    callback(response.ResponseHeader.ServiceResult, {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginBrowse(
      channel_.handle(),
      &request.RequestHeader,
      &request.View,
      request.RequestedMaxReferencesPerNode,
      descriptions.size(),
      descriptions.data(),
      &Request::OnComplete,
      async_request.release());

  request.NodesToBrowse = nullptr;
  request.NoOfNodesToBrowse = 0;

  if (!status_code)
    callback(status_code, {});
}

inline void Session::Read(Span<const OpcUa_ReadValueId> read_ids, const ReadCallback& callback) {
  ReadRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<ReadResponse>;
  auto async_request = std::make_unique<Request>([callback](ReadResponse& response) {
    callback(response.ResponseHeader.ServiceResult, {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginRead(
      channel_.handle(),
      &request.RequestHeader,
      request.MaxAge,
      request.TimestampsToReturn,
      read_ids.size(),
      read_ids.data(),
      &Request::OnComplete,
      async_request.release());

  request.NoOfNodesToRead = 0;
  request.NodesToRead = nullptr;

  if (!status_code)
    callback(status_code, {});
}

inline void Session::Reset() {
  std::lock_guard<std::mutex> lock{mutex_};
  subscriptions_.clear();
  acknowledgements_.clear();
  sent_acknowledgements_.clear();
  publishing_ = false;
}

inline void Session::InitRequestHeader(OpcUa_RequestHeader& header) const {
  std::lock_guard<std::mutex> lock{mutex_};
  header.TimeoutHint = 60000;
  header.Timestamp = ::OpcUa_DateTime_UtcNow();
  info_.authentication_token.CopyTo(header.AuthenticationToken);
}

inline void Session::StartPublishing(SubscriptionId subscription_id, NotificationHandler handler) {
  bool publishing = false;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    subscriptions_.emplace(subscription_id, std::move(handler));
    publishing = publishing_;
  }

  if (!publishing)
    Publish();
}

inline void Session::StopPublishing(SubscriptionId subscription_id) {
  std::lock_guard<std::mutex> lock{mutex_};
  subscriptions_.erase(subscription_id);
}

inline void Session::Publish() {
  std::vector<OpcUa_SubscriptionAcknowledgement> acknowledgements;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (publishing_)
      return;
    publishing_ = true;
    acknowledgements = std::move(acknowledgements_);
    acknowledgements_.clear();
    sent_acknowledgements_ = acknowledgements;
  }

  PublishRequest request;
  InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<PublishResponse>;
  auto async_request = std::make_unique<Request>([this](PublishResponse& response) {
    OnPublishResponse(response.ResponseHeader.ServiceResult,
        response.SubscriptionId,
        {response.AvailableSequenceNumbers, static_cast<size_t>(response.NoOfAvailableSequenceNumbers)},
        response.MoreNotifications != OpcUa_False,
        response.NotificationMessage,
        {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginPublish(
      channel_.handle(),
      &request.RequestHeader,
      acknowledgements.size(),
      acknowledgements.data(),
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    OnError(status_code);
}

inline void Session::OnPublishResponse(StatusCode status_code, SubscriptionId subscription_id,
    Span<SequenceNumber> available_sequence_numbers, bool more_notifications,
    OpcUa_NotificationMessage& message, Span<OpcUa_StatusCode> results) {
  if (!status_code) {
    OnError(status_code);
    return;
  }

  for (StatusCode result : results) {
    if (!result) {
      OnError(result);
      return;
    }
  }

  NotificationHandler handler;

  if (message.NoOfNotificationData) {
    std::lock_guard<std::mutex> lock{mutex_};
    assert(publishing_);
    publishing_ = false;
    sent_acknowledgements_.clear();
    acknowledgements_.push_back({subscription_id, message.SequenceNumber});

    auto i = subscriptions_.find(subscription_id);
    if (i != subscriptions_.end())
      handler = i->second;
  }

  Publish();

  if (handler)
    handler({message.NotificationData, static_cast<size_t>(message.NoOfNotificationData)});
}

inline void Session::OnError(StatusCode status_code) {
  // TODO:
  assert(false);
}

inline void Session::OnActivated(ByteString server_nonce) {
  bool has_subscriptions = false;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    //assert(!publishing_);
    info_.server_nonce = std::move(server_nonce);
    has_subscriptions = !subscriptions_.empty();
  }

  if (has_subscriptions)
    Publish();
}

} // namespace client
} // namespace opcua