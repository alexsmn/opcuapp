#include "opcua/server_session.h"

#include <algorithm>
#include <cstring>

namespace opcua {

namespace {

constexpr size_t kNotFound = static_cast<size_t>(-1);

}  // namespace

ServerSession::ServerSession(ServerSessionContext&& context)
    : ServerSessionContext{std::move(context)} {}

size_t ServerSession::ByteStringHash::operator()(
    const scada::ByteString& value) const {
  return std::hash<std::string_view>{}(
      std::string_view{value.data(), value.size()});
}

CreateSubscriptionResponse ServerSession::CreateSubscription(
    const CreateSubscriptionRequest& request) {
  return CreateSubscriptionWithId(next_subscription_id_++, request);
}

CreateSubscriptionResponse ServerSession::CreateSubscriptionWithId(
    SubscriptionId subscription_id,
    const CreateSubscriptionRequest& request) {
  next_subscription_id_ = std::max(next_subscription_id_, subscription_id + 1);
  auto subscription = std::make_unique<ServerSubscription>(
      subscription_id, request.parameters, this->executor,
      this->monitored_item_service, Now());

  subscriptions_.emplace(subscription_id, std::move(subscription));
  publish_order_.push_back(subscription_id);

  return {
      .status = scada::StatusCode::Good,
      .subscription_id = subscription_id,
      .revised_publishing_interval_ms =
          request.parameters.publishing_interval_ms,
      .revised_lifetime_count = request.parameters.lifetime_count,
      .revised_max_keep_alive_count = request.parameters.max_keep_alive_count};
}

ModifySubscriptionResponse ServerSession::ModifySubscription(
    const ModifySubscriptionRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = scada::StatusCode::Bad_WrongSubscriptionId};
  return subscription->Modify(request);
}

SetPublishingModeResponse ServerSession::SetPublishingMode(
    const SetPublishingModeRequest& request) {
  SetPublishingModeResponse response{.status = scada::StatusCode::Good};
  response.results.reserve(request.subscription_ids.size());

  for (const auto subscription_id : request.subscription_ids) {
    auto* subscription = FindSubscription(subscription_id);
    if (!subscription) {
      response.results.push_back(scada::StatusCode::Bad_WrongSubscriptionId);
      continue;
    }
    subscription->SetPublishingEnabled(request.publishing_enabled);
    response.results.push_back(scada::StatusCode::Good);
  }

  return response;
}

DeleteSubscriptionsResponse ServerSession::DeleteSubscriptions(
    const DeleteSubscriptionsRequest& request) {
  DeleteSubscriptionsResponse response{.status = scada::StatusCode::Good};
  response.results.reserve(request.subscription_ids.size());

  for (const auto subscription_id : request.subscription_ids) {
    if (!FindSubscription(subscription_id)) {
      response.results.push_back(scada::StatusCode::Bad_WrongSubscriptionId);
      continue;
    }
    EraseSubscription(subscription_id);
    response.results.push_back(scada::StatusCode::Good);
  }

  return response;
}

TransferSubscriptionsResponse ServerSession::TransferSubscriptionsFrom(
    ServerSession& source,
    const TransferSubscriptionsRequest& request) {
  TransferSubscriptionsResponse response{.status = scada::StatusCode::Good};
  response.results.reserve(request.subscription_ids.size());

  for (const auto subscription_id : request.subscription_ids) {
    if (FindSubscription(subscription_id)) {
      response.results.push_back(scada::StatusCode::Bad);
      continue;
    }

    auto source_it = source.subscriptions_.find(subscription_id);
    if (source_it == source.subscriptions_.end()) {
      response.results.push_back(scada::StatusCode::Bad_WrongSubscriptionId);
      continue;
    }

    subscriptions_.emplace(subscription_id, std::move(source_it->second));
    publish_order_.push_back(subscription_id);
    source.EraseSubscription(subscription_id);
    response.results.push_back(scada::StatusCode::Good);
  }

  RefreshNextSubscriptionId();
  return response;
}

CreateMonitoredItemsResponse ServerSession::CreateMonitoredItems(
    const CreateMonitoredItemsRequest& request) {
  if (request.items_to_create.size() >
      operation_limits.max_monitored_items_per_call) {
    return {.status = scada::StatusCode::Bad_TooManyMonitoredItems};
  }
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = scada::StatusCode::Bad_WrongSubscriptionId};
  return subscription->CreateMonitoredItems(request);
}

ModifyMonitoredItemsResponse ServerSession::ModifyMonitoredItems(
    const ModifyMonitoredItemsRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = scada::StatusCode::Bad_WrongSubscriptionId};
  return subscription->ModifyMonitoredItems(request);
}

DeleteMonitoredItemsResponse ServerSession::DeleteMonitoredItems(
    const DeleteMonitoredItemsRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = scada::StatusCode::Bad_WrongSubscriptionId};
  return subscription->DeleteMonitoredItems(request);
}

SetMonitoringModeResponse ServerSession::SetMonitoringMode(
    const SetMonitoringModeRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = scada::StatusCode::Bad_WrongSubscriptionId};
  return subscription->SetMonitoringMode(request);
}

std::vector<scada::StatusCode> ServerSession::AcknowledgePublishRequest(
    const PublishRequest& request) {
  std::vector<scada::StatusCode> ack_results(
      request.subscription_acknowledgements.size(), scada::StatusCode::Good);
  std::unordered_map<SubscriptionId,
                     std::vector<std::pair<size_t, scada::UInt32>>>
      grouped_acknowledgements;

  for (size_t i = 0; i < request.subscription_acknowledgements.size(); ++i) {
    const auto& acknowledgement = request.subscription_acknowledgements[i];
    if (!FindSubscription(acknowledgement.subscription_id)) {
      ack_results[i] = scada::StatusCode::Bad_WrongSubscriptionId;
      continue;
    }
    grouped_acknowledgements[acknowledgement.subscription_id].push_back(
        {i, acknowledgement.sequence_number});
  }

  for (const auto& [subscription_id, group] : grouped_acknowledgements) {
    auto* subscription = FindSubscription(subscription_id);
    if (!subscription)
      continue;

    std::vector<scada::UInt32> sequence_numbers;
    sequence_numbers.reserve(group.size());
    for (const auto& [index, sequence_number] : group)
      sequence_numbers.push_back(sequence_number);

    const auto results = subscription->Acknowledge(sequence_numbers);
    for (size_t i = 0; i < group.size(); ++i)
      ack_results[group[i].first] = results[i];
  }

  return ack_results;
}

ServerSession::PublishPollResult ServerSession::PollPublish() {
  const auto now_time = Now();
  const auto pending_index = FindNextReadySubscription(now_time, true);
  const auto publish_index = pending_index != kNotFound
                                 ? pending_index
                                 : FindNextReadySubscription(now_time, false);
  if (publish_index == kNotFound) {
    if (subscriptions_.empty()) {
      return {.response = PublishResponse{
                  .status = scada::StatusCode::Bad_NothingToDo}};
    }

    std::optional<base::Time> earliest_deadline;
    std::optional<base::TimeDelta> max_wait_before_recheck;
    for (const auto subscription_id : publish_order_) {
      auto* subscription = FindSubscription(subscription_id);
      if (!subscription)
        continue;
      subscription->PrimePublishCycle(now_time);
      const auto deadline = subscription->NextPublishDeadline();
      if (!deadline.has_value())
        continue;
      earliest_deadline =
          !earliest_deadline.has_value() || *deadline < *earliest_deadline
              ? deadline
              : earliest_deadline;
      max_wait_before_recheck =
          !max_wait_before_recheck.has_value() ||
                  subscription->PublishingInterval() < *max_wait_before_recheck
              ? subscription->PublishingInterval()
              : max_wait_before_recheck;
    }

    if (!earliest_deadline.has_value()) {
      return {.response = PublishResponse{.status = scada::StatusCode::Good}};
    }

    auto wait_for = std::max(base::TimeDelta{}, *earliest_deadline - now_time);
    if (max_wait_before_recheck.has_value()) {
      wait_for = std::min(wait_for, *max_wait_before_recheck);
    }
    return {.wait_for = wait_for};
  }

  const auto subscription_id = publish_order_[publish_index];
  auto* subscription = FindSubscription(subscription_id);
  if (!subscription) {
    return {.response = PublishResponse{.status = scada::StatusCode::Bad}};
  }

  auto published = subscription->TryPublish(now_time);
  if (!published.has_value()) {
    return {.response = PublishResponse{.status = scada::StatusCode::Good}};
  }

  AdvancePublishCursorAfter(publish_index);
  return {.response = std::move(published)};
}

PublishResponse ServerSession::Publish(const PublishRequest& request) {
  auto ack_results = AcknowledgePublishRequest(request);
  auto poll = PollPublish();
  if (!poll.response.has_value()) {
    return {.status = subscriptions_.empty()
                          ? scada::StatusCode::Bad_NothingToDo
                          : scada::StatusCode::Good,
            .results = std::move(ack_results)};
  }
  poll.response->results = std::move(ack_results);
  return *poll.response;
}

RepublishResponse ServerSession::Republish(
    const RepublishRequest& request) const {
  const auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = scada::StatusCode::Bad_WrongSubscriptionId};
  return subscription->Republish(request.retransmit_sequence_number);
}

BrowseResponse ServerSession::StoreBrowseResults(
    BrowseResponse response,
    size_t requested_max_references_per_node) {
  if (requested_max_references_per_node == 0)
    return response;

  for (auto& result : response.results) {
    result =
        PageBrowseResult(std::move(result), requested_max_references_per_node);
  }
  return response;
}

BrowseNextResponse ServerSession::BrowseNext(const BrowseNextRequest& request) {
  BrowseNextResponse response{.status = scada::StatusCode::Good};
  response.results.reserve(request.continuation_points.size());

  for (const auto& continuation_point : request.continuation_points) {
    const auto it = browse_continuations_.find(continuation_point);
    if (it == browse_continuations_.end()) {
      response.results.push_back(
          {.status_code = scada::StatusCode::Bad_WrongIndex});
      continue;
    }

    if (request.release_continuation_points) {
      browse_continuations_.erase(it);
      response.results.push_back({.status_code = scada::StatusCode::Good});
      continue;
    }

    response.results.push_back(ResumeBrowseResult(continuation_point));
  }

  return response;
}

std::vector<SubscriptionId> ServerSession::GetSubscriptionIds() const {
  std::vector<SubscriptionId> result;
  result.reserve(publish_order_.size());
  for (const auto subscription_id : publish_order_) {
    if (FindSubscription(subscription_id))
      result.push_back(subscription_id);
  }
  return result;
}

bool ServerSession::HasSubscription(SubscriptionId subscription_id) const {
  return FindSubscription(subscription_id) != nullptr;
}

ServerSubscription* ServerSession::FindSubscription(
    SubscriptionId subscription_id) {
  const auto it = subscriptions_.find(subscription_id);
  return it != subscriptions_.end() ? it->second.get() : nullptr;
}

const ServerSubscription* ServerSession::FindSubscription(
    SubscriptionId subscription_id) const {
  const auto it = subscriptions_.find(subscription_id);
  return it != subscriptions_.end() ? it->second.get() : nullptr;
}

void ServerSession::EraseSubscription(SubscriptionId subscription_id) {
  subscriptions_.erase(subscription_id);
  const auto it =
      std::find(publish_order_.begin(), publish_order_.end(), subscription_id);
  if (it == publish_order_.end())
    return;

  const auto index =
      static_cast<size_t>(std::distance(publish_order_.begin(), it));
  publish_order_.erase(it);
  if (publish_order_.empty()) {
    next_publish_index_ = 0;
    return;
  }
  if (index < next_publish_index_)
    --next_publish_index_;
  if (next_publish_index_ >= publish_order_.size())
    next_publish_index_ = 0;
}

void ServerSession::AdvancePublishCursorAfter(size_t index) {
  if (publish_order_.empty()) {
    next_publish_index_ = 0;
    return;
  }
  next_publish_index_ = (index + 1) % publish_order_.size();
}

size_t ServerSession::FindNextReadySubscription(base::Time now,
                                                bool require_pending) const {
  if (publish_order_.empty())
    return kNotFound;

  for (size_t offset = 0; offset < publish_order_.size(); ++offset) {
    const auto index = (next_publish_index_ + offset) % publish_order_.size();
    const auto subscription_id = publish_order_[index];
    const auto* subscription = FindSubscription(subscription_id);
    if (!subscription)
      continue;
    if (require_pending && !subscription->HasPendingNotifications())
      continue;
    if (!subscription->IsPublishReady(now))
      continue;
    return index;
  }
  return kNotFound;
}

void ServerSession::RefreshNextSubscriptionId() {
  for (const auto& [subscription_id, subscription] : subscriptions_)
    next_subscription_id_ =
        std::max(next_subscription_id_, subscription_id + 1);
}

scada::ByteString ServerSession::MakeBrowseContinuationPoint() {
  scada::ByteString value(sizeof(next_browse_continuation_id_), '\0');
  const auto raw = next_browse_continuation_id_++;
  std::memcpy(value.data(), &raw, sizeof(raw));
  return value;
}

scada::BrowseResult ServerSession::PageBrowseResult(
    scada::BrowseResult result,
    size_t requested_max_references_per_node) {
  result.continuation_point.clear();
  if (requested_max_references_per_node == 0 ||
      result.references.size() <= requested_max_references_per_node) {
    return result;
  }

  auto continuation_point = MakeBrowseContinuationPoint();
  BrowseContinuationState state;
  state.remaining_references.assign(
      std::make_move_iterator(
          result.references.begin() +
          static_cast<std::ptrdiff_t>(requested_max_references_per_node)),
      std::make_move_iterator(result.references.end()));
  browse_continuations_.emplace(continuation_point, std::move(state));
  result.references.resize(requested_max_references_per_node);
  result.continuation_point = std::move(continuation_point);
  return result;
}

scada::BrowseResult ServerSession::ResumeBrowseResult(
    const scada::ByteString& continuation_point) {
  auto it = browse_continuations_.find(continuation_point);
  if (it == browse_continuations_.end()) {
    return {.status_code = scada::StatusCode::Bad_WrongIndex};
  }

  scada::BrowseResult result;
  result.status_code = scada::StatusCode::Good;
  result.references = std::move(it->second.remaining_references);
  browse_continuations_.erase(it);
  return result;
}

}  // namespace opcua
