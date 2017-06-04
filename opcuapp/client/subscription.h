#pragma once

#include "opcuapp/client/session.h"
#include "opcuapp/extension_object.h"
#include "opcuapp/structs.h"
#include "opcuapp/timer.h"

#include <chrono>
#include <vector>

namespace opcua {
namespace client {

class MonitoredItem;

struct SubscriptionParams {
  std::chrono::milliseconds publishing_interval;
  unsigned lifetime_count;
  unsigned max_keepalive_count;
  unsigned max_notifications_per_publish;
  bool publishing_enabled;
  unsigned priority;
};

using DataChangeHandler = std::function<void(MonitoredItemNotification& notification)>;

class Subscription {
 public:
  explicit Subscription(Session& session);
  ~Subscription();

  SubscriptionId GetId() const;

  void Create();
  void Delete();

  // Monitored items.
  void Subscribe(MonitoredItem& monitored_item, ReadValueId read_id, DataChangeHandler handler);
  void Unsubscribe(MonitoredItem& monitored_item);

  Signal<void(StatusCode status_code)> status_changed;

 private:
  struct ItemState {
    ItemState(MonitoredItemClientHandle client_handle, ReadValueId read_id, DataChangeHandler data_change_handler)
        : client_handle{client_handle},
          read_id{std::move(read_id)},
          data_change_handler{std::move(data_change_handler)} {
    }

    const MonitoredItemClientHandle client_handle;
    const ReadValueId read_id;
    const DataChangeHandler data_change_handler;

    bool subscribed = false;
    bool added = false;
    MonitoredItemId id = 0;
  };

  using CreateMonitoredItemsCallback = std::function<void(StatusCode status_code,
      Span<OpcUa_MonitoredItemCreateResult> results)>;
  void CreateMonitoredItems(Span<const OpcUa_MonitoredItemCreateRequest> items,
      OpcUa_TimestampsToReturn return_timestamps, const CreateMonitoredItemsCallback& callback);

  using DeleteMonitoredItemsCallback = std::function<void(StatusCode status_code, Span<OpcUa_StatusCode> results)>;
  void DeleteMonitoredItems(Span<const MonitoredItemId> item_ids, const DeleteMonitoredItemsCallback& callback);

  void CommitCreate();
  void StartPublishing();

  void OnError(StatusCode status_code);
  void Reset();

  void ScheduleCommitItems();
  void ScheduleCommitItemsDone();
  void CommitItems();

  void CreateMonitoredItems();
  void OnCreateMonitoredItemsResponse(StatusCode status_code, Span<OpcUa_MonitoredItemCreateResult> results);

  void DeleteMonitoredItems();
  void OnDeleteMonitoredItemsResponse(StatusCode status_code, Span<OpcUa_StatusCode> results);

  void OnNotification(ExtensionObject& notification);

  Session& session_;

  ScopedSignalConnection session_status_connection_;

  mutable std::mutex mutex_;

  SubscriptionParams params_;

  bool creation_requested_ = false;
  bool created_ = false;
  SubscriptionId id_ = 0;
  bool publishing_ = false;

  bool commit_items_scheduled_ = false;

  std::map<MonitoredItemClientHandle, ItemState> item_states_;
  std::map<MonitoredItem*, ItemState*> items_;
  std::vector<ItemState*> pending_subscribe_items_;
  std::vector<ItemState*> subscribing_items_;
  std::vector<ItemState*> pending_unsubscribe_items_;
  std::vector<ItemState*> unsubscribing_items_;

  MonitoredItemClientHandle next_monitored_item_client_handle_ = 0;

  Timer commit_timer_;

  static const UInt32 kCommitDelayMs = 1000;
};

template<typename T>
inline bool Erase(std::vector<T>& vector, const T& item) {
  auto i = std::find(vector.begin(), vector.end(), item);
  if (i == vector.end())
    return false;
  auto p = --vector.end();
  if (i != p)
    *i = *p;
  vector.erase(p);
  return true;
}

template<typename T>
inline bool Contains(const std::vector<T>& vector, const T& item) {
  return std::find(vector.begin(), vector.end(), item) != vector.end();
}

// Subscription

Subscription::Subscription(Session& session)
    : session_{session},
      commit_timer_{[this] { ScheduleCommitItemsDone(); }} {
  session_status_connection_ = session_.status_changed.Connect([this](StatusCode status_code) {
    if (!status_code)
      return;
    if (creation_requested_ && !created_)
      CommitCreate();
    else if (created_)
      CommitItems();
  });
}

inline Subscription::~Subscription() {
  Delete();
}

inline void Subscription::Create() {
  creation_requested_ = true;
  if (session_.status_code())
    CommitCreate();
}

inline void Subscription::CommitCreate() {
  CreateSubscriptionRequest request;
  session_.InitRequestHeader(request.RequestHeader);
  request.RequestedPublishingInterval = static_cast<Double>(params_.publishing_interval.count());
  request.RequestedLifetimeCount = params_.lifetime_count;
  request.MaxNotificationsPerPublish = params_.max_notifications_per_publish;
  request.PublishingEnabled = params_.publishing_enabled ? OpcUa_True : OpcUa_False;
  request.Priority = params_.priority;

  using Request = AsyncRequest<CreateSubscriptionResponse>;
  auto async_request = std::make_unique<Request>([this](CreateSubscriptionResponse& response) {
    const StatusCode status_code{response.ResponseHeader.ServiceResult};
    if (status_code) {
      std::lock_guard<std::mutex> lock{mutex_};
      created_ = true;
      id_ = response.SubscriptionId;
    }
    StartPublishing();
    CommitItems();
    status_changed(status_code);
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
    status_changed(status_code);
}

inline void Subscription::Delete() {
  Reset();
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

  status_changed(status_code);
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

  session_status_connection_.Reset();

  if (publishing)
    session_.StopPublishing(id);
}

inline void Subscription::StartPublishing() {
  SubscriptionId id = 0;

  std::lock_guard<std::mutex> lock{mutex_};
  {
    if (publishing_)
      return;
    publishing_ = true;
    id = id_;
  }

  session_.StartPublishing(id, [this](Span<OpcUa_ExtensionObject> notifications) {
    for (auto& notification : notifications)
      OnNotification(ExtensionObject{std::move(notification)});
  });
}

inline void Subscription::Subscribe(MonitoredItem& monitored_item, ReadValueId read_id, DataChangeHandler handler) {
  assert(items_.find(&monitored_item) == items_.end());

  auto client_handle = next_monitored_item_client_handle_++;
  assert(item_states_.find(client_handle) == item_states_.end());
  auto p = item_states_.emplace(std::piecewise_construct,
      std::forward_as_tuple(client_handle),
      std::forward_as_tuple(client_handle, std::move(read_id), std::move(handler)));

  ItemState& item_state = p.first->second;
  item_state.subscribed = true;
  items_.emplace(&monitored_item, &item_state);

  assert(!Contains(pending_subscribe_items_, &item_state));
  pending_subscribe_items_.emplace_back(&item_state);

  ScheduleCommitItems();
}

inline void Subscription::Unsubscribe(MonitoredItem& monitored_item) {
  auto i = items_.find(&monitored_item);
  assert(i != items_.end());
  ItemState& item_state = *i->second;
  assert(item_state.subscribed);
  item_state.subscribed = false;
  items_.erase(i);

  assert(item_states_.find(item_state.client_handle) != item_states_.end());
  item_states_.erase(item_state.client_handle);

  if (Erase(pending_subscribe_items_, &item_state)) {
    items_.erase(i);
  } else if (Contains(subscribing_items_, &item_state)) {
    // Wait for the subscription end.
  } else if (item_state.added) {
    assert(!Contains(pending_unsubscribe_items_, &item_state));
    pending_unsubscribe_items_.emplace_back(&item_state);
    ScheduleCommitItems();
  } else {
    // Add failed.
    items_.erase(i);
  }
}

inline void Subscription::ScheduleCommitItems() {
  if (!created_ || commit_items_scheduled_)
    return;

  commit_items_scheduled_ = true;
  commit_timer_.Start(kCommitDelayMs);
}

inline void Subscription::ScheduleCommitItemsDone() {
  commit_items_scheduled_ = false;

  CommitItems();
}

inline void Subscription::CommitItems() {
  assert(created_);

  DeleteMonitoredItems();
  CreateMonitoredItems();
}

inline void Subscription::CreateMonitoredItems() {
  if (!subscribing_items_.empty())
    return;

  if (pending_subscribe_items_.empty())
    return;

  std::vector<opcua::MonitoredItemCreateRequest> requests(pending_subscribe_items_.size());

  for (size_t i = 0; i < requests.size(); ++i) {
    auto& item = *pending_subscribe_items_[i];
    auto& request = requests[i];

    opcua::ExtensionObject ext_filter;

    if (item.read_id.AttributeId != OpcUa_Attributes_EventNotifier) {
      opcua::DataChangeFilter filter;
      filter.DeadbandType = OpcUa_DeadbandType_None;
      filter.Trigger = OpcUa_DataChangeTrigger_StatusValueTimestamp;
      ext_filter = filter.Encode();

    } else {
      opcua::EventFilter filter;
      ext_filter = filter.Encode();
    }

    request.MonitoringMode = OpcUa_MonitoringMode_Reporting;
    request.RequestedParameters.ClientHandle = item.client_handle;
    Copy(item.read_id.NodeId, request.ItemToMonitor.NodeId);
    request.ItemToMonitor.AttributeId = item.read_id.AttributeId;
    ext_filter.Release(request.RequestedParameters.Filter);
  }

  subscribing_items_.swap(pending_subscribe_items_);

  CreateMonitoredItems({requests.data(), requests.size()}, OpcUa_TimestampsToReturn_Both,
      [this](opcua::StatusCode status_code, opcua::Span<OpcUa_MonitoredItemCreateResult> results) {
        OnCreateMonitoredItemsResponse(status_code, results);
      });
}

inline void Subscription::OnCreateMonitoredItemsResponse(StatusCode status_code, Span<OpcUa_MonitoredItemCreateResult> results) {
  if (!status_code)
    return OnError(status_code);

  if (results.size() != subscribing_items_.size())
    return OnError(OpcUa_Bad);

  auto items = std::move(subscribing_items_);
  subscribing_items_.clear();

  for (size_t i = 0; i < results.size(); ++i) {
    auto& result = results[i];
    auto& item = *items[i];
    assert(!item.added);
    if (OpcUa_IsGood(result.StatusCode)) {
      item.id = result.MonitoredItemId;
      item.added = true;
      if (item.subscribed) {
        // TODO: Forward status.
      } else {
        assert(!Contains(unsubscribing_items_, &item));
        unsubscribing_items_.emplace_back(&item);
        // Commit will be done immediately.
      }
    } else {
      if (item.subscribed) {
        if (item.data_change_handler) {
          MonitoredItemNotification notification;
          notification.ClientHandle = item.client_handle;
          notification.Value.StatusCode = result.StatusCode;
          item.data_change_handler(std::move(notification));
        }
      } else {
        assert(item_states_.find(item.client_handle) != item_states_.end());
        item_states_.erase(item.client_handle);
      }
    }
  }

  CommitItems();
}

inline void Subscription::DeleteMonitoredItems() {
  if (!unsubscribing_items_.empty())
    return;

  if (pending_unsubscribe_items_.empty())
    return;

  std::vector<opcua::MonitoredItemId> item_ids(pending_unsubscribe_items_.size());
  for (size_t i = 0; i < item_ids.size(); ++i) {
    auto& item = *pending_unsubscribe_items_[i];
    assert(item.added);
    item_ids[i] = item.id;
  }

  unsubscribing_items_.swap(pending_unsubscribe_items_);

  DeleteMonitoredItems({item_ids.data(), item_ids.size()},
      [this](opcua::StatusCode status_code, opcua::Span<OpcUa_StatusCode> results) {
        OnDeleteMonitoredItemsResponse(status_code, results);
      });
}

inline void Subscription::OnDeleteMonitoredItemsResponse(StatusCode status_code, Span<OpcUa_StatusCode> results) {
  if (!status_code)
    return OnError(status_code);

  if (results.size() != unsubscribing_items_.size())
    return OnError(OpcUa_Bad);

  auto items = std::move(unsubscribing_items_);
  unsubscribing_items_.clear();

  for (size_t i = 0; i < results.size(); ++i) {
    auto result = results[i];
    auto& item = *items[i];
    assert(!item.subscribed);
    assert(item.added);
    if (OpcUa_IsGood(result)) {
      assert(item_states_.find(item.client_handle) != item_states_.end());
      item_states_.erase(item.client_handle);
    } else {
      // Unsubscription failed. Unexpected.
      assert(false);
      OnError(result);
      return;
    }
  }

  CommitItems();
}

inline void Subscription::OnNotification(ExtensionObject& notification) {
  if (notification.type_id() == OpcUaId_StatusChangeNotification_Encoding_DefaultBinary) {
    auto& status_change_notification = *static_cast<OpcUa_StatusChangeNotification*>(notification.object());
    status_changed(status_change_notification.Status);
    // TODO: Reset

  } else if (notification.type_id() == OpcUaId_DataChangeNotification_Encoding_DefaultBinary) {
    auto& data_change_notification = *static_cast<OpcUa_DataChangeNotification*>(notification.object());
    for (auto& item_notification : MakeSpan(data_change_notification.MonitoredItems, data_change_notification.NoOfMonitoredItems)) {
      auto i = item_states_.find(item_notification.ClientHandle);
      if (i != item_states_.end())
        i->second.data_change_handler(MonitoredItemNotification{std::move(item_notification)});
    }
  }
}

} // namespace client
} // namespace opcua