#include "opcua/client/client_channel.h"

#include "opcua/base/boost_log.h"
#include "opcua/types/co_result.h"

#include <boost/asio/error.hpp>

#include <memory>
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
                                            ua::SetPublishingModeRequest>) {
          return "SetPublishingMode";
        } else if constexpr (std::is_same_v<Request,
                                            ua::DeleteSubscriptionsRequest>) {
          return "DeleteSubscriptions";
        } else if constexpr (std::is_same_v<Request, PublishRequest>) {
          return "Publish";
        } else if constexpr (std::is_same_v<Request, RepublishRequest>) {
          return "Republish";
        } else if constexpr (std::is_same_v<Request,
                                            ua::TransferSubscriptionsRequest>) {
          return "TransferSubscriptions";
        } else if constexpr (std::is_same_v<Request,
                                            CreateMonitoredItemsRequest>) {
          return "CreateMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            ModifyMonitoredItemsRequest>) {
          return "ModifyMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            ua::DeleteMonitoredItemsRequest>) {
          return "DeleteMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            ua::SetMonitoringModeRequest>) {
          return "SetMonitoringMode";
        } else if constexpr (std::is_same_v<Request, ua::ReadRequest>) {
          return "Read";
        } else if constexpr (std::is_same_v<Request, ua::WriteRequest>) {
          return "Write";
        } else if constexpr (std::is_same_v<Request, ua::BrowseRequest>) {
          return "Browse";
        } else if constexpr (std::is_same_v<Request, ua::BrowseNextRequest>) {
          return "BrowseNext";
        } else if constexpr (std::is_same_v<
                                 Request,
                                 ua::TranslateBrowsePathsToNodeIdsRequest>) {
          return "TranslateBrowsePaths";
        } else if constexpr (std::is_same_v<Request, ua::CallRequest>) {
          return "Call";
        } else if constexpr (std::is_same_v<Request, ua::HistoryReadRequest>) {
          return "HistoryRead";
        } else if constexpr (std::is_same_v<Request, ua::AddNodesRequest>) {
          return "AddNodes";
        } else if constexpr (std::is_same_v<Request, ua::DeleteNodesRequest>) {
          return "DeleteNodes";
        } else if constexpr (std::is_same_v<Request,
                                            ua::AddReferencesRequest>) {
          return "AddReferences";
        } else if constexpr (std::is_same_v<Request,
                                            ua::DeleteReferencesRequest>) {
          return "DeleteReferences";
        }
        return "";
      },
      request);
}

}  // namespace

ClientChannel::ClientChannel(Context context)
    : executor_{std::move(context.executor)},
      connection_{context.connection},
      authentication_token_{std::move(context.authentication_token)},
      request_timeout_{context.request_timeout},
      endpoint_url_{std::move(context.endpoint_url)} {}

std::uint32_t ClientChannel::NextRequestHandle() {
  return next_request_handle_++;
}

void ClientChannel::set_authentication_token(NodeId token) {
  authentication_token_ = std::move(token);
}

void ClientChannel::MarkLoginComplete() {
  login_complete_ = true;
}

CoStatusOr<ResponseBody> ClientChannel::Call(std::uint32_t request_handle,
                                             RequestBody request,
                                             std::string trace_parent) {
  auto request_id = co_await Send(request_handle, std::move(request),
                                  std::move(trace_parent));
  if (!request_id.ok()) {
    co_return StatusOr<ResponseBody>{request_id.status()};
  }
  co_return co_await Receive(*request_id, request_handle, request_timeout_);
}

CoStatusOr<std::uint32_t> ClientChannel::Send(std::uint32_t request_handle,
                                              RequestBody request,
                                              std::string trace_parent) {
  if (!login_complete_ && !IsPreLoginRequest(request)) {
    LOG_WARNING(logger_) << "OPC UA request sent before login completed: "
                         << RequestName(request)
                         << LOG_TAG("RequestHandle", request_handle)
                         << LOG_TAG("AuthenticationToken",
                                    authentication_token_.ToString())
                         << LOG_TAG("Endpoint", endpoint_url_);
  }

  const auto request_name = RequestName(request);
  co_await WaitForSendTurn();
  // Released on every exit from this function, exceptions included. The two
  // explicit ReleaseSendTurn() calls this replaced covered every `co_return`
  // and not one throw -- and the write path CAN throw: after a write deadline
  // has torn the transport down, the next WriteFrame built its timer on the
  // empty executor the reset transport reports and raised `bad_executor`.
  // That escaped here with the turn still held, the coroutine that owned it
  // was detached (so nothing logged), and every later Send on the channel
  // parked in WaitForSendTurn for ever with no deadline armed -- which is what
  // the aggregating proxy's `Disconnect` step wedging for twelve hours
  // actually was (backlog 647). The transport now refuses that write rather
  // than throwing; this guard is what makes the next such throw, whatever
  // raises it, cost one request instead of the channel.
  const SendTurnGuard turn{*this};
  // Renew the security token only while the channel is QUIET: the Renew
  // handshake reads its response directly off the transport, and the response
  // read loop runs whenever responses are pending — two concurrent readers
  // steal each other's frames (observed as decode failures poisoning every
  // pending request when a renewal landed mid-traffic). Holding the send turn
  // with no pending responses guarantees this coroutine is the only reader.
  // Deferring while busy is safe: the periodic liveness probe soon provides a
  // quiet send, and the server keeps accepting the previous token during the
  // switchover (OPC UA Part 6 §6.7.4).
  if (pending_responses_.empty() && connection_.ShouldRenewSecurityToken()) {
    const auto renew_status = co_await connection_.RenewSecurityToken();
    if (renew_status.bad()) {
      LOG_WARNING(logger_) << "OPC UA security-token renewal failed"
                           << LOG_TAG("Status", ToString(renew_status))
                           << LOG_TAG("Endpoint", endpoint_url_);
      co_return StatusOr<std::uint32_t>{renew_status};
    }
  }
  const std::uint32_t request_id = connection_.NextRequestId();
  const auto send_status = co_await connection_.SendRequest(
      request_id,
      RequestMessage{.request_handle = request_handle,
                     .body = std::move(request),
                     .trace_parent = std::move(trace_parent)},
      authentication_token_);
  if (send_status.bad()) {
    LOG_WARNING(logger_) << "OPC UA request send failed: " << request_name
                         << LOG_TAG("RequestId", request_id)
                         << LOG_TAG("RequestHandle", request_handle)
                         << LOG_TAG("Status", ToString(send_status))
                         << LOG_TAG("Endpoint", endpoint_url_);
    co_return StatusOr<std::uint32_t>{send_status};
  }
  co_return StatusOr<std::uint32_t>{request_id};
}

CoStatusOr<ResponseBody> ClientChannel::Receive(
    std::uint32_t request_id,
    std::uint32_t request_handle,
    std::optional<std::chrono::steady_clock::duration> timeout) {
  // Fail fast on a stream that has already failed. This is not an
  // optimisation: EnsureReadLoop() below refuses to run on a dead stream, so
  // without this the coroutine would register a pending entry and wait on a
  // gate nothing can ever complete -- for ever when the caller passed no
  // timeout, which is exactly Publish's case.
  //
  // Bad_NoCommunication rather than the frame's own status for the reason
  // given in RunReadLoop: conversion.h maps it to scada::Bad_Disconnected,
  // which is what makes the reconnect loop treat this as a dead link.
  if (stream_failed_) {
    co_return StatusOr<ResponseBody>{Status{StatusCode::Bad_NoCommunication}};
  }

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

  // Bound the wait when the caller asked for it. Without this a peer that
  // stopped answering but left the socket open blocks here for ever: the read
  // loop is itself parked in ReadResponse() waiting for bytes that never come,
  // so FailPendingResponses — its only wakeup — never runs.
  //
  // The deadline releases this coroutine and leaves the gate open, so the only
  // parties that settle it are DeliverResponse and FailPendingResponses. An
  // answer that lands between the deadline firing and this resuming is
  // therefore delivered into `pending` like any other, and returned below.
  bool answered = true;
  if (timeout) {
    answered = co_await pending->ready.WaitFor(*timeout);
  } else {
    co_await pending->ready.Wait();
  }

  const bool timed_out = !answered && !pending->response;
  if (timed_out) {
    pending->response = StatusOr<ResponseBody>{Status{StatusCode::Bad_Timeout}};
  }

  if (!pending->response) {
    pending_responses_.erase(request_id);
    co_return StatusOr<ResponseBody>{Status{StatusCode::Bad}};
  }

  auto response = std::move(*pending->response);
  pending_responses_.erase(request_id);
  // A deadline abandons the request rather than cancelling it — the peer was
  // never told, and may still answer. Remember the id so DeliverResponse drops
  // that answer instead of buffering it for a Receive that will never come.
  if (timed_out) {
    abandoned_responses_.insert(request_id);
  }
  co_return std::move(response);
}

void ClientChannel::EnsureReadLoop() {
  // Never re-enter the loop on a stream that has already failed. This is the
  // guard that actually stops backlog 705's spin: Receive() calls this
  // directly, so suppressing only the restart at the end of RunReadLoop would
  // leave every new request starting a fresh read that fails on the same
  // undecodable bytes.
  //
  // Send is deliberately NOT guarded. A best-effort teardown -- Delete-
  // MonitoredItems, DeleteSubscriptions, CloseSession -- should still be
  // attempted on the way out; refusing those buys nothing and leaves session
  // state on the peer until it times out.
  if (read_loop_running_ || stream_failed_) {
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
                           << LOG_TAG("Status",
                                      ToString(response_frame.status()))
                           << LOG_TAG(
                                  "PendingCount",
                                  static_cast<int>(pending_responses_.size()))
                           << LOG_TAG("Endpoint", endpoint_url_);
      // A frame read that failed leaves the stream desynchronised: whatever is
      // at the head of it could not be decoded, and nothing here consumes it,
      // so every later read fails on the same bytes. This channel is finished.
      //
      // Two things follow, and omitting either one produced backlog 705 -- a
      // proxy that sat for four days with 86 bytes stuck in Recv-Q on two
      // downstreams, logging this very line every five seconds.
      //
      //  - Do not restart the loop. The old code fell through to
      //    EnsureReadLoop() below, so each new request re-entered a read that
      //    could only fail again, for ever, on a socket nobody was draining.
      //  - Report a CONNECTIVITY failure, not the frame's own status. The
      //    read failed with a bare Bad, and bare Bad is not in
      //    IsConnectivityFailure (maintain_redundant_connection.h), so the
      //    reconnect loop above read it as "the service said no" rather than
      //    "the link is gone" and never reconnected. Bad_NoCommunication is
      //    what this actually is; common/opcua_bridge/conversion.h maps it to
      //    scada::Bad_Disconnected, which IS in that predicate, so reporting
      //    it makes the layer that OWNS the connection replace it -- this
      //    channel only holds a reference and must not tear it down itself.
      stream_failed_ = true;
      FailPendingResponses(Status{StatusCode::Bad_NoCommunication});
      break;
    }

    DeliverResponse(std::move(*response_frame));
  }

  read_loop_running_ = false;
  if (!stream_failed_ && !pending_responses_.empty()) {
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
      it != pending_responses_.end() && !it->second->ready.completed()) {
    auto pending = std::move(it->second);
    pending_responses_.erase(it);
    if (frame.message.request_handle != pending->request_handle) {
      LOG_WARNING(logger_) << "OPC UA response request handle mismatch"
                           << LOG_TAG("RequestId", request_id)
                           << LOG_TAG("ExpectedRequestHandle",
                                      pending->request_handle)
                           << LOG_TAG("ActualRequestHandle",
                                      frame.message.request_handle)
                           << LOG_TAG("Endpoint", endpoint_url_);
      pending->response = StatusOr<ResponseBody>{Status{StatusCode::Bad}};
    } else {
      pending->response = StatusOr<ResponseBody>{std::move(frame.message.body)};
    }
    pending->ready.Complete();
    return;
  }

  // The caller of a timed-out request has gone; buffering its late answer would
  // retain it until the channel dies, since no Receive will ever claim it.
  if (auto it = abandoned_responses_.find(request_id);
      it != abandoned_responses_.end()) {
    abandoned_responses_.erase(it);
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
    // Nothing but this function and DeliverResponse settles a pending entry,
    // and DeliverResponse removes the entry before it does, so this should
    // never fire. It stays because a second Complete() on a one-shot gate is
    // a panic rather than a no-op.
    if (response->ready.completed()) {
      continue;
    }
    response->response = StatusOr<ResponseBody>{status};
    response->ready.Complete();
  }
}

}  // namespace opcua
