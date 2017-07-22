#pragma once

#include "opcuapp/client/session.h"
#include "opcuapp/extension_object.h"

#include <chrono>
#include <vector>

namespace opcua {
namespace client {

struct SubscriptionParams {
  std::chrono::milliseconds publishing_interval;
  unsigned lifetime_count;
  unsigned max_keepalive_count;
  unsigned max_notifications_per_publish;
  bool publishing_enabled;
  unsigned priority;
};

class Subscription {
 public:
  explicit Subscription(Session& session);
  ~Subscription();

  SubscriptionId GetId() const;

  using CreateSubscriptionCallback = std::function<void(StatusCode status_code)>;
  void Create(const SubscriptionParams& params, const CreateSubscriptionCallback& callback);

  using StatusChangeHandler = std::function<void(StatusCode status_code)>;
  using DataChangeHandler = std::function<void(OpcUa_DataChangeNotification& notification)>;
  void StartPublishing(StatusChangeHandler status_change_handler, DataChangeHandler data_change_handler);
  void StopPublishing();

  using CreateMonitoredItemsCallback = std::function<void(StatusCode status_code,
      Span<OpcUa_MonitoredItemCreateResult> results)>;
  void CreateMonitoredItems(Span<const OpcUa_MonitoredItemCreateRequest> items,
      OpcUa_TimestampsToReturn return_timestamps, const CreateMonitoredItemsCallback& callback);

  using DeleteMonitoredItemsCallback = std::function<void(StatusCode status_code, Span<OpcUa_StatusCode> results)>;
  void DeleteMonitoredItems(Span<const MonitoredItemId> item_ids, const DeleteMonitoredItemsCallback& callback);

  void Reset();

 private:
  void OnError(StatusCode status_code);

  Session& session_;
  DataChangeHandler data_change_handler_;
  StatusChangeHandler status_change_handler_;

  mutable std::mutex mutex_;
  SubscriptionId id_ = 0;
  bool publishing_ = false;
};

inline Subscription::Subscription(Session& session)
    : session_{session} {
}

inline Subscription::~Subscription() {
  Reset();
}

inline void Subscription::Create(const SubscriptionParams& params, const CreateSubscriptionCallback& callback) {
  CreateSubscriptionRequest request;
  session_.InitRequestHeader(request.RequestHeader);
  request.RequestedPublishingInterval = static_cast<opcua::Double>(params.publishing_interval.count());
  request.RequestedLifetimeCount = params.lifetime_count;
  request.MaxNotificationsPerPublish = params.max_notifications_per_publish;
  request.PublishingEnabled = params.publishing_enabled ? OpcUa_True : OpcUa_False;
  request.Priority = params.priority;

  using Request = AsyncRequest<CreateSubscriptionResponse>;
  auto async_request = std::make_unique<Request>([this, callback](CreateSubscriptionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code) {
      std::lock_guard<std::mutex> lock{mutex_};
      id_ = response.SubscriptionId;
    }
    callback(status_code);
  });

  StatusCode status_code = OpcUa_ClientApi_BeginCreateSubscription(
      session_.channel().handle(),
      &request.RequestHeader,
      request.RequestedPublishingInterval,
      request.RequestedLifetimeCount,
      request.RequestedMaxKeepAliveCount,
      request.MaxNotificationsPerPublish,
      request.PublishingEnabled,
      request.Priority,
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    callback(status_code);
}

inline void Subscription::CreateMonitoredItems(Span<const OpcUa_MonitoredItemCreateRequest> items,
    OpcUa_TimestampsToReturn return_timestamps, const CreateMonitoredItemsCallback& callback) {
  CreateMonitoredItemsRequest request;
  session_.InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<CreateMonitoredItemsResponse>;
  auto async_request = std::make_unique<Request>([this, callback](CreateMonitoredItemsResponse& response) {
    callback(response.ResponseHeader.ServiceResult, {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginCreateMonitoredItems(
      session_.channel().handle(),
      &request.RequestHeader,
      GetId(),
      return_timestamps,
      items.size(),
      items.data(),
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    callback(status_code, {});
}

inline void Subscription::DeleteMonitoredItems(Span<const MonitoredItemId> item_ids, const DeleteMonitoredItemsCallback& callback) {
  DeleteMonitoredItemsRequest request;
  session_.InitRequestHeader(request.RequestHeader);

  using Request = AsyncRequest<DeleteMonitoredItemsResponse>;
  auto async_request = std::make_unique<Request>([this, callback](DeleteMonitoredItemsResponse& response) {
    callback(response.ResponseHeader.ServiceResult, {response.Results, static_cast<size_t>(response.NoOfResults)});
  });

  StatusCode status_code = OpcUa_ClientApi_BeginDeleteMonitoredItems(
      session_.channel().handle(),
      &request.RequestHeader,
      GetId(),
      item_ids.size(),
      item_ids.data(),
      &Request::OnComplete,
      async_request.release());

  if (!status_code)
    callback(status_code, {});
}

inline SubscriptionId Subscription::GetId() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return id_;
}

inline void Subscription::OnError(StatusCode status_code) {
  Reset();

  if (status_change_handler_) {
    auto handler = std::move(status_change_handler_);
    status_change_handler_ = nullptr;
    handler(status_code);
  }
}

inline void Subscription::Reset() {
  bool publishing = false;
  SubscriptionId id = 0;

  std::lock_guard<std::mutex> lock{mutex_};
  {
    id = id_;
    publishing = publishing_;

    id_ = 0;
    publishing_ = false;
  }

  if (publishing)
    session_.StopPublishing(id);
}

inline void Subscription::StartPublishing(StatusChangeHandler status_change_handler, DataChangeHandler data_change_handler) {
  SubscriptionId id = 0;

  std::lock_guard<std::mutex> lock{mutex_};
  {
    if (publishing_)
      return;
    publishing_ = true;
    id = id_;
  }

  session_.StartPublishing(id, [status_change_handler, data_change_handler](StatusCode status_code, Span<OpcUa_ExtensionObject> notifications) {
    if (!status_code) {
      if (status_change_handler)
        status_change_handler(status_code);
      return;
    }

    for (auto& raw_notification : notifications) {
      ExtensionObject notification{std::move(raw_notification)};
      if (notification.type_id() == OpcUaId_StatusChangeNotification_Encoding_DefaultBinary) {
        auto& status_change_notification = *static_cast<OpcUa_StatusChangeNotification*>(notification.object());
        if (status_change_handler)
          status_change_handler(status_change_notification.Status);

      } else if (notification.type_id() == OpcUaId_DataChangeNotification_Encoding_DefaultBinary) {
        auto& data_change_notification = *static_cast<OpcUa_DataChangeNotification*>(notification.object());
        if (data_change_handler)
          data_change_handler(data_change_notification);
      }
    }
  });
}

} // namespace client
} // namespace opcua