#pragma once

#include <memory>
#include <map>
#include <mutex>
#include <opcuapp/client/async_request.h>
#include <opcuapp/client/channel.h>
#include <opcuapp/requests.h>
#include <opcuapp/structs.h>
#include <opcuapp/status_code.h>
#include <opcuapp/node_id.h>
#include <vector>

namespace opcua {
namespace client {

struct SessionInfo {
  NodeId session_id;
  NodeId authentication_token;
  Double revised_timeout;
  ByteString server_nonce;
  ByteString server_certificate;
};

class Session {
 public:
  explicit Session(Channel& channel) : core_{std::make_shared<Core>(channel)} {}

  Channel& channel() { return core_->channel_; }

  using CreateCallback = std::function<void(StatusCode status_code)>;
  void Create(const OpcUa_CreateSessionRequest& request, const CreateCallback& callback);

  using ActivateCallback = std::function<void(StatusCode status_code)>;
  void Activate(const ActivateCallback& callback);

  using CloseCallback = std::function<void(StatusCode status_code)>;
  void Close(const OpcUa_CloseSessionRequest& request, const CloseCallback& callback);

  using BrowseCallback = std::function<void(StatusCode status_code, Span<OpcUa_BrowseResult> results)>;
  void Browse(Span<const OpcUa_BrowseDescription> descriptions, const BrowseCallback& callback);

  using ReadCallback = std::function<void(StatusCode status_code, Span<OpcUa_DataValue> results)>;
  void Read(Span<const OpcUa_ReadValueId> read_ids, const ReadCallback& callback);

  using NotificationHandler = std::function<void(StatusCode status_code, Span<OpcUa_ExtensionObject> notifications)>;
  void StartPublishing(SubscriptionId subscription_id, NotificationHandler handler);
  void StopPublishing(SubscriptionId subscription_id);

  void InitRequestHeader(OpcUa_RequestHeader& header) const;

  void Reset();

 private:
  struct Core : public std::enable_shared_from_this<Core> {
    explicit Core(Channel& channel) : channel_{channel} {}

    void InitRequestHeader(OpcUa_RequestHeader& header) const;

    void OnActivated(ByteString server_nonce);
    void OnError(StatusCode status_code);

    void Publish();
    void OnPublishResponse(StatusCode status_code, SubscriptionId subscription_id,
        Span<SequenceNumber> available_sequence_numbers, bool more_notifications,
        OpcUa_NotificationMessage& notification_message, Span<OpcUa_StatusCode> results);

    Channel& channel_;

    mutable std::mutex mutex_;
    SessionInfo info_;
    std::map<SubscriptionId, NotificationHandler> subscriptions_;
    std::vector<OpcUa_SubscriptionAcknowledgement> acknowledgements_;
    std::vector<OpcUa_SubscriptionAcknowledgement> sent_acknowledgements_;
    bool publishing_ = false;
  };

  std::shared_ptr<Core> core_;
};

inline void Session::Create(const OpcUa_CreateSessionRequest& request, const CreateCallback& callback) {
  using Request = AsyncRequest<CreateSessionResponse>;
  auto async_request = std::make_unique<Request>([core = core_, callback](CreateSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code) {
      std::lock_guard<std::mutex> lock{core->mutex_};
      core->info_.session_id.swap(response.SessionId);
      core->info_.authentication_token.swap(response.AuthenticationToken);
      core->info_.revised_timeout = response.RevisedSessionTimeout;
      core->info_.server_nonce.swap(response.ServerNonce);
      core->info_.server_certificate.swap(response.ServerCertificate);
    }
    callback(status_code);
  });

  StatusCode status_code = OpcUa_ClientApi_BeginCreateSession(
      core_->channel_.handle(),
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
  auto async_request = std::make_unique<Request>([core = core_, callback](ActivateSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code)
      core->OnActivated(std::move(response.ServerNonce));
    callback(status_code);
  });

  StatusCode status_code = OpcUa_ClientApi_BeginActivateSession(
      core_->channel_.handle(),
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

inline void Session::Close(const OpcUa_CloseSessionRequest& request, const CloseCallback& callback) {
  using Request = AsyncRequest<CloseSessionResponse>;
  auto async_request = std::make_unique<Request>([core = core_, callback](CloseSessionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code) {
      std::lock_guard<std::mutex> lock{core->mutex_};
      core->info_ = SessionInfo{};
    }
    callback(status_code);
  });

  StatusCode status_code = OpcUa_ClientApi_BeginCloseSession(
      core_->channel_.handle(),
      &request.RequestHeader,
      request.DeleteSubscriptions,
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
      core_->channel_.handle(),
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
      core_->channel_.handle(),
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
  std::lock_guard<std::mutex> lock{core_->mutex_};
  core_->subscriptions_.clear();
  core_->acknowledgements_.clear();
  core_->sent_acknowledgements_.clear();
  core_->publishing_ = false;
}

inline void Session::InitRequestHeader(OpcUa_RequestHeader& header) const {
  core_->InitRequestHeader(header);
}

inline void Session::Core::InitRequestHeader(OpcUa_RequestHeader& header) const {
  std::lock_guard<std::mutex> lock{mutex_};
  header.TimeoutHint = 60000;
  header.Timestamp = ::OpcUa_DateTime_UtcNow();
  info_.authentication_token.CopyTo(header.AuthenticationToken);
}

inline void Session::StartPublishing(SubscriptionId subscription_id, NotificationHandler handler) {
  bool publishing = false;
  {
    std::lock_guard<std::mutex> lock{core_->mutex_};
    assert(core_->subscriptions_.find(subscription_id) == core_->subscriptions_.end());
    core_->subscriptions_.emplace(subscription_id, std::move(handler));
    publishing = core_->publishing_;
  }

  if (!publishing)
    core_->Publish();
}

inline void Session::StopPublishing(SubscriptionId subscription_id) {
  std::lock_guard<std::mutex> lock{core_->mutex_};
  core_->subscriptions_.erase(subscription_id);
}

inline void Session::Core::Publish() {
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
  auto async_request = std::make_unique<Request>([core = shared_from_this()](PublishResponse& response) {
    core->OnPublishResponse(response.ResponseHeader.ServiceResult,
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

inline void Session::Core::OnPublishResponse(StatusCode status_code, SubscriptionId subscription_id,
    Span<SequenceNumber> available_sequence_numbers, bool more_notifications,
    OpcUa_NotificationMessage& message, Span<OpcUa_StatusCode> results) {
  if (!status_code) {
    OnError(status_code);
    return;
  }

  if (results.size() != sent_acknowledgements_.size()) {
    OnError(OpcUa_Bad);
    return;
  }

  NotificationHandler handler;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    assert(publishing_);
    publishing_ = false;

    // TODO: Optimize.
    for (size_t i = 0; i < sent_acknowledgements_.size(); ++i) {
      if (!OpcUa_IsGood(results[i]))
        acknowledgements_.insert(acknowledgements_.begin(), sent_acknowledgements_[i]);
    }

    sent_acknowledgements_.clear();
    acknowledgements_.push_back({subscription_id, message.SequenceNumber});

    auto i = subscriptions_.find(subscription_id);
    if (i != subscriptions_.end())
      handler = i->second;
  }

  // Ensure the session isn't deleted from within of |handler|.
  auto ref = shared_from_this();

  if (handler)
    handler(status_code, {message.NotificationData, static_cast<size_t>(message.NoOfNotificationData)});

  Publish();
}

inline void Session::Core::OnError(StatusCode status_code) {
  NotificationHandler handler;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    publishing_ = false;
    acknowledgements_.insert(acknowledgements_.begin(), sent_acknowledgements_.begin(), sent_acknowledgements_.end());
  }
  if (handler)
    handler(status_code, {});
}

inline void Session::Core::OnActivated(ByteString server_nonce) {
  bool has_subscriptions = false;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    info_.server_nonce = std::move(server_nonce);
    has_subscriptions = !subscriptions_.empty();
  }

  if (has_subscriptions)
    Publish();
}

} // namespace client
} // namespace opcua