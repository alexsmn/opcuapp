#include "opcua/client/client_channel.h"

#include "opcua/base/boost_log.h"

#include <utility>
#include <variant>

namespace opcua {
namespace {

BoostLogger logger_{LOG_NAME("ClientChannel")};

bool IsPreLoginRequest(const RequestBody& request) {
  return std::holds_alternative<FindServersRequest>(request) ||
         std::holds_alternative<GetEndpointsRequest>(request) ||
         std::holds_alternative<CreateSessionRequest>(request) ||
         std::holds_alternative<ActivateSessionRequest>(request);
}

const char* RequestName(const RequestBody& request) {
  return std::visit(
      [](const auto& typed_request) -> const char* {
        using Request = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<Request, FindServersRequest>) {
          return "FindServers";
        } else if constexpr (std::is_same_v<Request, GetEndpointsRequest>) {
          return "GetEndpoints";
        } else if constexpr (std::is_same_v<Request, CreateSessionRequest>) {
          return "CreateSession";
        } else if constexpr (std::is_same_v<Request, ActivateSessionRequest>) {
          return "ActivateSession";
        } else if constexpr (std::is_same_v<Request, CloseSessionRequest>) {
          return "CloseSession";
        } else if constexpr (std::is_same_v<Request,
                                            CreateSubscriptionRequest>) {
          return "CreateSubscription";
        } else if constexpr (std::is_same_v<Request,
                                            ModifySubscriptionRequest>) {
          return "ModifySubscription";
        } else if constexpr (std::is_same_v<Request,
                                            SetPublishingModeRequest>) {
          return "SetPublishingMode";
        } else if constexpr (std::is_same_v<Request,
                                            DeleteSubscriptionsRequest>) {
          return "DeleteSubscriptions";
        } else if constexpr (std::is_same_v<Request, PublishRequest>) {
          return "Publish";
        } else if constexpr (std::is_same_v<Request, RepublishRequest>) {
          return "Republish";
        } else if constexpr (std::is_same_v<Request,
                                            TransferSubscriptionsRequest>) {
          return "TransferSubscriptions";
        } else if constexpr (std::is_same_v<Request,
                                            CreateMonitoredItemsRequest>) {
          return "CreateMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            ModifyMonitoredItemsRequest>) {
          return "ModifyMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            DeleteMonitoredItemsRequest>) {
          return "DeleteMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            SetMonitoringModeRequest>) {
          return "SetMonitoringMode";
        } else if constexpr (std::is_same_v<Request, ReadRequest>) {
          return "Read";
        } else if constexpr (std::is_same_v<Request, WriteRequest>) {
          return "Write";
        } else if constexpr (std::is_same_v<Request, BrowseRequest>) {
          return "Browse";
        } else if constexpr (std::is_same_v<Request, BrowseNextRequest>) {
          return "BrowseNext";
        } else if constexpr (std::is_same_v<Request,
                                            TranslateBrowsePathsRequest>) {
          return "TranslateBrowsePaths";
        } else if constexpr (std::is_same_v<Request, CallRequest>) {
          return "Call";
        } else if constexpr (std::is_same_v<Request, HistoryReadRawRequest>) {
          return "HistoryReadRaw";
        } else if constexpr (std::is_same_v<Request,
                                            HistoryReadEventsRequest>) {
          return "HistoryReadEvents";
        } else if constexpr (std::is_same_v<Request, AddNodesRequest>) {
          return "AddNodes";
        } else if constexpr (std::is_same_v<Request, DeleteNodesRequest>) {
          return "DeleteNodes";
        } else if constexpr (std::is_same_v<Request, AddReferencesRequest>) {
          return "AddReferences";
        } else if constexpr (std::is_same_v<Request, DeleteReferencesRequest>) {
          return "DeleteReferences";
        }
      },
      request);
}

}  // namespace

ClientChannel::ClientChannel(Context context)
    : executor_{std::move(context.executor)},
      connection_{context.connection},
      authentication_token_{std::move(context.authentication_token)} {}

std::uint32_t ClientChannel::NextRequestHandle() {
  return next_request_handle_++;
}

void ClientChannel::set_authentication_token(NodeId token) {
  authentication_token_ = std::move(token);
}

void ClientChannel::MarkLoginComplete() {
  login_complete_ = true;
}

Awaitable<StatusOr<ResponseBody>> ClientChannel::Call(
    std::uint32_t request_handle,
    RequestBody request) {
  auto request_id = co_await Send(request_handle, std::move(request));
  if (!request_id.ok()) {
    co_return StatusOr<ResponseBody>{request_id.status()};
  }
  co_return co_await Receive(*request_id, request_handle);
}

Awaitable<StatusOr<std::uint32_t>> ClientChannel::Send(
    std::uint32_t request_handle,
    RequestBody request) {
  if (!login_complete_ && !IsPreLoginRequest(request)) {
    LOG_WARNING(logger_) << "OPC UA request sent before login completed: "
                         << RequestName(request)
                         << LOG_TAG("RequestHandle", request_handle)
                         << LOG_TAG("AuthenticationToken",
                                    authentication_token_.ToString());
  }

  const auto request_name = RequestName(request);
  co_await WaitForSendTurn();
  const std::uint32_t request_id = connection_.NextRequestId();
  const auto send_status = co_await connection_.SendRequest(
      request_id,
      RequestMessage{.request_handle = request_handle,
                     .body = std::move(request)},
      authentication_token_);
  ReleaseSendTurn();
  if (send_status.bad()) {
    LOG_WARNING(logger_) << "OPC UA request send failed: " << request_name
                         << LOG_TAG("RequestId", request_id)
                         << LOG_TAG("RequestHandle", request_handle)
                         << LOG_TAG("Status", send_status);
    co_return StatusOr<std::uint32_t>{send_status};
  }
  co_return StatusOr<std::uint32_t>{request_id};
}

Awaitable<StatusOr<ResponseBody>> ClientChannel::Receive(
    std::uint32_t request_id,
    std::uint32_t request_handle) {
  if (auto it = buffered_responses_.find(request_id);
      it != buffered_responses_.end()) {
    if (it->second.request_handle != request_handle) {
      co_return StatusOr<ResponseBody>{Status{StatusCode::Bad}};
    }
    auto body = std::move(it->second.body);
    buffered_responses_.erase(it);
    co_return StatusOr<ResponseBody>{std::move(body)};
  }

  auto [pending_it, inserted] = pending_responses_.emplace(
      request_id, std::make_shared<PendingResponse>(executor_));
  auto pending = pending_it->second;
  pending->request_handle = request_handle;

  EnsureReadLoop();
  co_await pending->ready.Wait();

  if (!pending->response) {
    pending_responses_.erase(request_id);
    co_return StatusOr<ResponseBody>{Status{StatusCode::Bad}};
  }

  auto response = std::move(*pending->response);
  pending_responses_.erase(request_id);
  co_return std::move(response);
}

void ClientChannel::EnsureReadLoop() {
  if (read_loop_running_) {
    return;
  }

  read_loop_running_ = true;
  CoSpawn(executor_, [this]() -> Awaitable<void> { co_await RunReadLoop(); });
}

Awaitable<void> ClientChannel::RunReadLoop() {
  while (!pending_responses_.empty()) {
    auto response_frame = co_await connection_.ReadResponse();
    if (!response_frame.ok()) {
      LOG_WARNING(logger_) << "OPC UA response read failed"
                           << LOG_TAG("Status", response_frame.status())
                           << LOG_TAG("PendingCount",
                                      pending_responses_.size());
      FailPendingResponses(response_frame.status());
      break;
    }

    DeliverResponse(std::move(*response_frame));
  }

  read_loop_running_ = false;
  if (!pending_responses_.empty()) {
    EnsureReadLoop();
  }
  co_return;
}

Awaitable<void> ClientChannel::WaitForSendTurn() {
  for (;;) {
    if (!send_in_progress_) {
      send_in_progress_ = true;
      co_return;
    }

    base::AsyncCompletion waiter{executor_};
    send_waiters_.push_back(waiter);
    co_await waiter.Wait();
  }
}

void ClientChannel::ReleaseSendTurn() {
  send_in_progress_ = false;
  if (send_waiters_.empty()) {
    return;
  }

  auto waiter = send_waiters_.front();
  send_waiters_.pop_front();
  waiter.Complete();
}

void ClientChannel::DeliverResponse(ClientResponseFrame frame) {
  const auto request_id = frame.request_id;
  if (auto it = pending_responses_.find(request_id);
      it != pending_responses_.end()) {
    auto pending = std::move(it->second);
    pending_responses_.erase(it);
    if (frame.message.request_handle != pending->request_handle) {
      LOG_WARNING(logger_) << "OPC UA response request handle mismatch"
                           << LOG_TAG("RequestId", request_id)
                           << LOG_TAG("ExpectedRequestHandle",
                                      pending->request_handle)
                           << LOG_TAG("ActualRequestHandle",
                                      frame.message.request_handle);
      pending->response = StatusOr<ResponseBody>{Status{StatusCode::Bad}};
    } else {
      pending->response = StatusOr<ResponseBody>{std::move(frame.message.body)};
    }
    pending->ready.Complete();
    return;
  }

  buffered_responses_.emplace(
      request_id,
      BufferedResponse{.request_handle = frame.message.request_handle,
                       .body = std::move(frame.message.body)});
}

void ClientChannel::FailPendingResponses(Status status) {
  auto pending = std::move(pending_responses_);
  pending_responses_.clear();
  for (auto& [request_id, response] : pending) {
    response->response = StatusOr<ResponseBody>{status};
    response->ready.Complete();
  }
}

}  // namespace opcua
