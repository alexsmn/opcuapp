#include "opcua/client_protocol_subscription.h"

#include <utility>
#include <variant>

namespace opcua {
namespace {

constexpr std::size_t kMaxOutstandingPublishRequests = 2;

template <typename Response>
scada::StatusOr<Response> NarrowResponse(
    scada::StatusOr<ResponseBody> result) {
  if (!result.ok()) {
    return scada::StatusOr<Response>{result.status()};
  }
  if (auto* fault = std::get_if<ServiceFault>(&result.value())) {
    return scada::StatusOr<Response>{fault->status};
  }
  if (auto* typed = std::get_if<Response>(&result.value())) {
    return scada::StatusOr<Response>{std::move(*typed)};
  }
  return scada::StatusOr<Response>{scada::Status{scada::StatusCode::Bad}};
}

}  // namespace

ClientProtocolSubscription::ClientProtocolSubscription(
    ClientChannel& channel)
    : channel_{channel} {}

Awaitable<scada::Status> ClientProtocolSubscription::Create(
    SubscriptionParameters parameters) {
  const auto handle = channel_.NextRequestHandle();
  auto result = co_await channel_.Call(
      handle, RequestBody{CreateSubscriptionRequest{
                  .parameters = std::move(parameters)}});
  auto narrowed = NarrowResponse<CreateSubscriptionResponse>(
      std::move(result));
  if (!narrowed.ok()) {
    co_return narrowed.status();
  }
  if (narrowed->status.bad()) {
    co_return narrowed->status;
  }
  subscription_id_ = narrowed->subscription_id;
  is_created_ = true;
  co_return scada::Status{scada::StatusCode::Good};
}

Awaitable<scada::StatusOr<
    ClientProtocolSubscription::CreateMonitoredItemResult>>
ClientProtocolSubscription::CreateMonitoredItem(
    scada::ReadValueId read_value_id,
    MonitoringParameters params,
    DataChangeHandler handler) {
  if (!is_created_) {
    co_return scada::StatusOr<CreateMonitoredItemResult>{
        scada::Status{scada::StatusCode::Bad}};
  }
  const scada::UInt32 client_handle = next_client_handle_++;
  params.client_handle = client_handle;

  const auto request_handle = channel_.NextRequestHandle();
  auto result = co_await channel_.Call(
      request_handle,
      RequestBody{CreateMonitoredItemsRequest{
          .subscription_id = subscription_id_,
          .timestamps_to_return = TimestampsToReturn::Both,
          .items_to_create = {MonitoredItemCreateRequest{
              .item_to_monitor = std::move(read_value_id),
              .monitoring_mode = MonitoringMode::Reporting,
              .requested_parameters = std::move(params),
          }},
      }});
  auto narrowed = NarrowResponse<CreateMonitoredItemsResponse>(
      std::move(result));
  if (!narrowed.ok()) {
    co_return scada::StatusOr<CreateMonitoredItemResult>{narrowed.status()};
  }
  if (narrowed->status.bad() || narrowed->results.empty()) {
    co_return scada::StatusOr<CreateMonitoredItemResult>{
        narrowed->results.empty()
            ? scada::Status{scada::StatusCode::Bad}
            : narrowed->status};
  }
  const auto& item_result = narrowed->results.front();
  if (item_result.status.bad()) {
    co_return scada::StatusOr<CreateMonitoredItemResult>{item_result.status};
  }
  handlers_.emplace(client_handle, std::move(handler));
  client_handle_by_item_id_.emplace(item_result.monitored_item_id,
                                     client_handle);
  co_return scada::StatusOr<CreateMonitoredItemResult>{
      CreateMonitoredItemResult{
          .monitored_item_id = item_result.monitored_item_id,
          .client_handle = client_handle,
      }};
}

Awaitable<scada::Status> ClientProtocolSubscription::DeleteMonitoredItem(
    MonitoredItemId monitored_item_id) {
  if (!is_created_) {
    co_return scada::Status{scada::StatusCode::Bad};
  }
  const auto handle = channel_.NextRequestHandle();
  auto result = co_await channel_.Call(
      handle, RequestBody{DeleteMonitoredItemsRequest{
                  .subscription_id = subscription_id_,
                  .monitored_item_ids = {monitored_item_id},
              }});
  auto narrowed = NarrowResponse<DeleteMonitoredItemsResponse>(
      std::move(result));
  if (!narrowed.ok()) {
    co_return narrowed.status();
  }
  // Drop the handler regardless of server response; on a bad response the
  // server may or may not have removed it, but our client state is
  // unambiguous.
  if (auto it = client_handle_by_item_id_.find(monitored_item_id);
      it != client_handle_by_item_id_.end()) {
    handlers_.erase(it->second);
    client_handle_by_item_id_.erase(it);
  }
  co_return narrowed->status;
}

Awaitable<scada::Status> ClientProtocolSubscription::SendPublishRequest() {
  auto acks = std::move(pending_acks_);
  pending_acks_.clear();

  const auto handle = channel_.NextRequestHandle();
  auto request_id = co_await channel_.Send(
      handle, RequestBody{PublishRequest{
                  .subscription_acknowledgements = std::move(acks)}});
  if (!request_id.ok()) {
    pending_acks_.insert(pending_acks_.begin(), acks.begin(), acks.end());
    co_return request_id.status();
  }
  outstanding_publishes_.push_back(OutstandingPublish{
      .request_id = *request_id,
      .request_handle = handle,
  });
  co_return scada::Status{scada::StatusCode::Good};
}

Awaitable<scada::Status> ClientProtocolSubscription::FillPublishWindow() {
  while (outstanding_publishes_.size() < kMaxOutstandingPublishRequests) {
    const auto status = co_await SendPublishRequest();
    if (status.bad()) {
      co_return status;
    }
  }
  co_return scada::Status{scada::StatusCode::Good};
}

scada::Status ClientProtocolSubscription::HandlePublishResponse(
    PublishResponse response) {
  if (response.status.bad()) {
    return response.status;
  }

  // Queue ack for the sequence number we just received so it goes out on
  // the next PublishRequest. Keep-alive responses carry sequence number 0
  // and do not need acknowledgement.
  if (response.notification_message.sequence_number != 0) {
    pending_acks_.push_back(SubscriptionAcknowledgement{
        .subscription_id = response.subscription_id,
        .sequence_number = response.notification_message.sequence_number,
    });
  }

  // Dispatch data-change notifications to handlers by client_handle.
  for (const auto& data : response.notification_message.notification_data) {
    if (const auto* change = std::get_if<DataChangeNotification>(&data)) {
      for (const auto& item : change->monitored_items) {
        if (auto it = handlers_.find(item.client_handle);
            it != handlers_.end() && it->second) {
          it->second(item.value);
        }
      }
    }
    // Event + StatusChange notifications are intentionally not wired up in
    // this revision; the existing client didn't consume them either.
  }
  return scada::Status{scada::StatusCode::Good};
}

Awaitable<scada::Status> ClientProtocolSubscription::Publish() {
  if (!is_created_) {
    co_return scada::Status{scada::StatusCode::Bad};
  }
  const auto fill_status = co_await FillPublishWindow();
  if (fill_status.bad()) {
    co_return fill_status;
  }

  const auto published = outstanding_publishes_.front();
  outstanding_publishes_.pop_front();
  auto result =
      co_await channel_.Receive(published.request_id, published.request_handle);
  auto narrowed = NarrowResponse<PublishResponse>(std::move(result));
  if (!narrowed.ok()) {
    co_return narrowed.status();
  }
  co_return HandlePublishResponse(std::move(*narrowed));
}

Awaitable<scada::Status> ClientProtocolSubscription::Delete() {
  if (!is_created_) {
    co_return scada::Status{scada::StatusCode::Good};
  }
  const auto handle = channel_.NextRequestHandle();
  auto result = co_await channel_.Call(
      handle, RequestBody{DeleteSubscriptionsRequest{
                  .subscription_ids = {subscription_id_}}});
  is_created_ = false;
  handlers_.clear();
  client_handle_by_item_id_.clear();
  pending_acks_.clear();
  outstanding_publishes_.clear();
  auto narrowed = NarrowResponse<DeleteSubscriptionsResponse>(
      std::move(result));
  if (!narrowed.ok()) {
    co_return narrowed.status();
  }
  co_return narrowed->status;
}

}  // namespace opcua
