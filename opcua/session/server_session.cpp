#include "opcua/session/server_session.h"

#include <algorithm>
#include <cstring>

namespace opcua {

namespace {

constexpr size_t kNotFound = static_cast<size_t>(-1);

}  // namespace

ServerSession::ServerSession(ServerSessionContext&& context)
    : ServerSessionContext{std::move(context)} {}

size_t ServerSession::ByteStringHash::operator()(
    const ByteString& value) const {
  return std::hash<std::string_view>{}(
      std::string_view{value.data(), value.size()});
}

CreateSubscriptionResponse ServerSession::CreateSubscription(
    const CreateSubscriptionRequest& request,
    std::string trace_parent) {
  return CreateSubscriptionWithId(next_subscription_id_++, request,
                                  std::move(trace_parent));
}

CreateSubscriptionResponse ServerSession::CreateSubscriptionWithId(
    SubscriptionId subscription_id,
    const CreateSubscriptionRequest& request,
    std::string trace_parent) {
  next_subscription_id_ = std::max(next_subscription_id_, subscription_id + 1);
  auto subscription = std::make_unique<ServerSubscription>(
      subscription_id, request.parameters, this->executor,
      this->create_subscription, Now(), std::move(trace_parent));
  // The subscription revised the requested parameters to the server's limits;
  // report the revised values back to the client.
  const auto& revised = subscription->parameters();

  subscriptions_.emplace(subscription_id, std::move(subscription));
  publish_order_.push_back(subscription_id);

  return {.status = StatusCode::Good,
          .subscription_id = subscription_id,
          .revised_publishing_interval_ms = revised.publishing_interval_ms,
          .revised_lifetime_count = revised.lifetime_count,
          .revised_max_keep_alive_count = revised.max_keep_alive_count};
}

ModifySubscriptionResponse ServerSession::ModifySubscription(
    const ModifySubscriptionRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = StatusCode::Bad_SubscriptionIdInvalid};
  return subscription->Modify(request);
}

ua::SetPublishingModeResponse ServerSession::SetPublishingMode(
    const ua::SetPublishingModeRequest& request) {
  ua::SetPublishingModeResponse response;
  response.results.reserve(request.subscription_ids.size());

  for (const auto subscription_id : request.subscription_ids) {
    auto* subscription = FindSubscription(subscription_id);
    if (!subscription) {
      response.results.push_back(Status{StatusCode::Bad_SubscriptionIdInvalid});
      continue;
    }
    subscription->SetPublishingEnabled(request.publishing_enabled);
    response.results.push_back(Status{StatusCode::Good});
  }

  return response;
}

ua::DeleteSubscriptionsResponse ServerSession::DeleteSubscriptions(
    const ua::DeleteSubscriptionsRequest& request) {
  // Migrated to the generated response type: the service result now lives in
  // the ResponseHeader, and per-item results are full Status values.
  ua::DeleteSubscriptionsResponse response;
  response.results.reserve(request.subscription_ids.size());

  for (const auto subscription_id : request.subscription_ids) {
    if (!FindSubscription(subscription_id)) {
      response.results.push_back(Status{StatusCode::Bad_SubscriptionIdInvalid});
      continue;
    }
    EraseSubscription(subscription_id);
    response.results.push_back(Status{StatusCode::Good});
  }

  return response;
}

ua::TransferSubscriptionsResponse ServerSession::TransferSubscriptionsFrom(
    ServerSession& source,
    const ua::TransferSubscriptionsRequest& request) {
  // The generated response carries a TransferResult per subscription; this
  // implementation does not track availableSequenceNumbers, so each result's
  // sequence-number list is left empty.
  ua::TransferSubscriptionsResponse response;
  response.results.reserve(request.subscription_ids.size());
  const auto push_result = [&](StatusCode status) {
    response.results.push_back(
        ua::TransferResult{.status_code = Status{status}});
  };

  for (const auto subscription_id : request.subscription_ids) {
    if (FindSubscription(subscription_id)) {
      push_result(StatusCode::Bad);
      continue;
    }

    auto source_it = source.subscriptions_.find(subscription_id);
    if (source_it == source.subscriptions_.end()) {
      push_result(StatusCode::Bad_SubscriptionIdInvalid);
      continue;
    }

    subscriptions_.emplace(subscription_id, std::move(source_it->second));
    publish_order_.push_back(subscription_id);
    source.EraseSubscription(subscription_id);
    push_result(StatusCode::Good);
  }

  RefreshNextSubscriptionId();
  return response;
}

CreateMonitoredItemsResponse ServerSession::CreateMonitoredItems(
    const CreateMonitoredItemsRequest& request) {
  if (request.items_to_create.size() >
      operation_limits.max_monitored_items_per_call) {
    return {.status = StatusCode::Bad_TooManyMonitoredItems};
  }
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = StatusCode::Bad_SubscriptionIdInvalid};
  return subscription->CreateMonitoredItems(request);
}

ModifyMonitoredItemsResponse ServerSession::ModifyMonitoredItems(
    const ModifyMonitoredItemsRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = StatusCode::Bad_SubscriptionIdInvalid};
  return subscription->ModifyMonitoredItems(request);
}

ua::DeleteMonitoredItemsResponse ServerSession::DeleteMonitoredItems(
    const ua::DeleteMonitoredItemsRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription) {
    ua::DeleteMonitoredItemsResponse response;
    response.response_header.service_result =
        Status{StatusCode::Bad_SubscriptionIdInvalid};
    return response;
  }
  return subscription->DeleteMonitoredItems(request);
}

ua::SetMonitoringModeResponse ServerSession::SetMonitoringMode(
    const ua::SetMonitoringModeRequest& request) {
  auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription) {
    ua::SetMonitoringModeResponse response;
    response.response_header.service_result =
        Status{StatusCode::Bad_SubscriptionIdInvalid};
    return response;
  }
  return subscription->SetMonitoringMode(request);
}

std::vector<StatusCode> ServerSession::AcknowledgePublishRequest(
    const PublishRequest& request) {
  std::vector<StatusCode> ack_results(
      request.subscription_acknowledgements.size(), StatusCode::Good);
  std::unordered_map<SubscriptionId, std::vector<std::pair<size_t, UInt32>>>
      grouped_acknowledgements;

  for (size_t i = 0; i < request.subscription_acknowledgements.size(); ++i) {
    const auto& acknowledgement = request.subscription_acknowledgements[i];
    if (!FindSubscription(acknowledgement.subscription_id)) {
      ack_results[i] = StatusCode::Bad_SubscriptionIdInvalid;
      continue;
    }
    grouped_acknowledgements[acknowledgement.subscription_id].push_back(
        {i, acknowledgement.sequence_number});
  }

  for (const auto& [subscription_id, group] : grouped_acknowledgements) {
    auto* subscription = FindSubscription(subscription_id);
    if (!subscription)
      continue;

    std::vector<UInt32> sequence_numbers;
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
      // OPC UA Part 4 §5.13.5 Publish: a Publish for a session with no
      // subscriptions is answered with Bad_NoSubscription.
      // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
      return {.response =
                  PublishResponse{.status = StatusCode::Bad_NoSubscription}};
    }

    std::optional<DateTime> earliest_deadline;
    std::optional<Duration> max_wait_before_recheck;
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
      return {.response = PublishResponse{.status = StatusCode::Good}};
    }

    auto wait_for = std::max(Duration{}, *earliest_deadline - now_time);
    if (max_wait_before_recheck.has_value()) {
      wait_for = std::min(wait_for, *max_wait_before_recheck);
    }
    return {.wait_for = wait_for};
  }

  const auto subscription_id = publish_order_[publish_index];
  auto* subscription = FindSubscription(subscription_id);
  if (!subscription) {
    return {.response = PublishResponse{.status = StatusCode::Bad}};
  }

  auto published = subscription->TryPublish(now_time);
  if (!published.has_value()) {
    return {.response = PublishResponse{.status = StatusCode::Good}};
  }

  AdvancePublishCursorAfter(publish_index);
  return {.response = std::move(published)};
}

PublishResponse ServerSession::Publish(const PublishRequest& request) {
  auto ack_results = AcknowledgePublishRequest(request);
  auto poll = PollPublish();
  if (!poll.response.has_value()) {
    return {.status = subscriptions_.empty() ? StatusCode::Bad_NoSubscription
                                             : StatusCode::Good,
            .results = std::move(ack_results)};
  }
  poll.response->results = std::move(ack_results);
  return *poll.response;
}

RepublishResponse ServerSession::Republish(
    const RepublishRequest& request) const {
  const auto* subscription = FindSubscription(request.subscription_id);
  if (!subscription)
    return {.status = StatusCode::Bad_SubscriptionIdInvalid};
  return subscription->Republish(request.retransmit_sequence_number);
}

ua::BrowseResponse ServerSession::StoreBrowseResults(
    ua::BrowseResponse response,
    size_t requested_max_references_per_node) {
  if (requested_max_references_per_node == 0)
    return response;

  for (auto& result : response.results) {
    result =
        PageBrowseResult(std::move(result), requested_max_references_per_node);
  }
  return response;
}

ua::BrowseNextResponse ServerSession::BrowseNext(
    const ua::BrowseNextRequest& request) {
  ua::BrowseNextResponse response;
  response.response_header.service_result = StatusCode::Good;
  response.results.reserve(request.continuation_points.size());

  for (const auto& continuation_point : request.continuation_points) {
    const auto it = browse_continuations_.find(continuation_point);
    if (it == browse_continuations_.end()) {
      response.results.push_back(
          {.status_code = Status{StatusCode::Bad_ContinuationPointInvalid}});
      continue;
    }

    if (request.release_continuation_points) {
      browse_continuations_.erase(it);
      response.results.push_back({.status_code = Status{StatusCode::Good}});
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

size_t ServerSession::FindNextReadySubscription(DateTime now,
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

ByteString ServerSession::MakeBrowseContinuationPoint() {
  ByteString value(sizeof(next_browse_continuation_id_), '\0');
  const auto raw = next_browse_continuation_id_++;
  std::memcpy(value.data(), &raw, sizeof(raw));
  return value;
}

ua::BrowseResult ServerSession::PageBrowseResult(
    ua::BrowseResult result,
    size_t requested_max_references_per_node) {
  result.continuation_point.clear();
  if (requested_max_references_per_node == 0 ||
      result.references.size() <= requested_max_references_per_node) {
    return result;
  }

  // Cannot allocate another continuation point once the per-session limit is
  // reached (OPC UA Part 4 §5.8.2,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.2). The client
  // must free continuation points (BrowseNext with releaseContinuationPoints)
  // before browsing more.
  if (browse_continuations_.size() >= kMaxBrowseContinuationPoints) {
    return {.status_code = Status{StatusCode::Bad_NoContinuationPoints}};
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

ua::BrowseResult ServerSession::ResumeBrowseResult(
    const ByteString& continuation_point) {
  auto it = browse_continuations_.find(continuation_point);
  if (it == browse_continuations_.end()) {
    return {.status_code = Status{StatusCode::Bad_ContinuationPointInvalid}};
  }

  ua::BrowseResult result;
  result.status_code = Status{StatusCode::Good};
  result.references = std::move(it->second.remaining_references);
  browse_continuations_.erase(it);
  return result;
}

}  // namespace opcua
