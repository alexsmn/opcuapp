#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/async_completion.h"
#include "opcua/base/awaitable.h"
#include "opcua/client/client_connection.h"
#include "opcua/message.h"
#include "opcua/types/co_result.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace opcua {

// Default deadline for a request-response call on a client channel. A peer that
// stops answering without closing the socket (a half-open connection) otherwise
// blocks the caller for ever: nothing below this layer bounds the wait, because
// the read loop is itself waiting for bytes that never arrive.
//
// The value is a compromise. It has to outlast the slowest legitimate
// request-response service — a wide HistoryRead, or a user Method that does
// real work — while still being short enough that a wedged downstream is
// noticed and dropped rather than pinning a coroutine and its connection.
// Raise it via Context::request_timeout on a channel that carries such
// services; Publish, the one service whose latency is unbounded by design,
// does not come through here at all.
inline constexpr auto kDefaultClientRequestTimeout = std::chrono::seconds{30};

// Request-response correlation layer shared by OPC UA client transports.
class ClientChannel {
 public:
  struct Context {
    AnyExecutor executor;
    ClientConnection& connection;
    // Authentication token from a successful CreateSession; empty node id
    // before session activation.
    NodeId authentication_token;
    // Deadline applied by Call(); see kDefaultClientRequestTimeout. Publish is
    // deliberately exempt because it uses the split Send/Receive API below.
    std::chrono::steady_clock::duration request_timeout =
        kDefaultClientRequestTimeout;
    // The peer this channel talks to, as the URL the session connected to.
    // Carried only so every warning the channel emits can name it as
    // `Endpoint`. That tag is load-bearing rather than cosmetic: when a
    // downstream stops draining its socket, the reconnect loop above logs
    // nothing (the link still looks connected) and this channel's warnings
    // are the only lines the process emits about it -- backlog 705 was four
    // days of `response read failed` naming no peer, and the stuck downstream
    // had to be identified from the proxy's socket table. Empty is allowed
    // and yields an empty tag.
    std::string endpoint_url;
  };

  explicit ClientChannel(Context context);

  // Allocates a fresh request handle (monotonically increasing).
  [[nodiscard]] std::uint32_t NextRequestHandle();

  // Sets the session authentication token to attach to subsequent requests.
  // Called after a successful ActivateSession.
  void set_authentication_token(NodeId token);
  void MarkLoginComplete();
  [[nodiscard]] const NodeId& authentication_token() const {
    return authentication_token_;
  }
  // The endpoint URL this channel was created for; see Context::endpoint_url.
  [[nodiscard]] const std::string& endpoint_url() const {
    return endpoint_url_;
  }

  // Sends `request` and awaits the matching response. The returned body's
  // concrete type depends on the request; callers typically `std::get` or
  // `std::visit` on the variant. `trace_parent` optionally carries a W3C
  // traceparent in the request header (see RequestMessage::trace_parent).
  [[nodiscard]] CoStatusOr<ResponseBody> Call(std::uint32_t request_handle,
                                              RequestBody request,
                                              std::string trace_parent = {});

  // Lower-level split send/receive API for callers that keep multiple
  // requests outstanding (Publish). `Receive` buffers unrelated responses
  // for later matching by request_id.
  //
  // `timeout` bounds the wait, answering Bad_Timeout if the peer does not. It
  // defaults to none because this is Publish's path: OPC UA Part 4 §5.14.5
  // Publish (https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5)
  // has the server hold a Publish request until data is available, so a
  // deadline here would break subscriptions. Call() supplies one instead.
  [[nodiscard]] CoStatusOr<std::uint32_t> Send(std::uint32_t request_handle,
                                               RequestBody request,
                                               std::string trace_parent = {});
  [[nodiscard]] CoStatusOr<ResponseBody> Receive(
      std::uint32_t request_id,
      std::uint32_t request_handle,
      std::optional<std::chrono::steady_clock::duration> timeout =
          std::nullopt);

 private:
  struct BufferedResponse {
    std::uint32_t request_handle = 0;
    ResponseBody body;
  };

  struct PendingResponse {
    explicit PendingResponse(AnyExecutor executor)
        : ready{std::move(executor)} {}

    std::uint32_t request_handle = 0;
    // Settled by DeliverResponse or FailPendingResponses, and by nothing else:
    // Receive's deadline is `AsyncCompletion::WaitFor`, which releases the
    // waiter and leaves this gate open.
    base::AsyncCompletion ready;
    std::optional<StatusOr<ResponseBody>> response;
  };

  void EnsureReadLoop();
  [[nodiscard]] Awaitable<void> RunReadLoop();
  [[nodiscard]] Awaitable<void> WaitForSendTurn();
  void ReleaseSendTurn();
  void DeliverResponse(ClientResponseFrame frame);
  void FailPendingResponses(Status status);

  AnyExecutor executor_;
  ClientConnection& connection_;
  NodeId authentication_token_;
  std::uint32_t next_request_handle_ = 1;
  std::chrono::steady_clock::duration request_timeout_;
  std::string endpoint_url_;
  std::unordered_map<std::uint32_t, BufferedResponse> buffered_responses_;
  // Request ids whose caller timed out and stopped waiting. The peer may still
  // answer, and that answer must be dropped rather than buffered for a Receive
  // that will never come.
  std::unordered_set<std::uint32_t> abandoned_responses_;
  std::unordered_map<std::uint32_t, std::shared_ptr<PendingResponse>>
      pending_responses_;
  bool read_loop_running_ = false;

  // Set when a frame read fails, which means the byte stream is desynchronised
  // and this channel can never be used again -- see RunReadLoop. The channel
  // holds its connection by reference and cannot tear it down (only the owner
  // may; destroying the transport is how this codebase cancels a read in
  // flight), so the recovery it CAN drive is to refuse further use and report
  // a connectivity failure, which is what makes the layer above reconnect.
  bool stream_failed_ = false;
  bool login_complete_ = false;
  bool send_in_progress_ = false;
  std::deque<base::AsyncCompletion> send_waiters_;
};

}  // namespace opcua
