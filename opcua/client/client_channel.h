#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/base/async_completion.h"
#include "opcua/base/awaitable.h"
#include "opcua/client/client_connection.h"
#include "opcua/message.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>

namespace opcua {

// Request-response correlation layer shared by OPC UA client transports.
class ClientChannel {
 public:
  struct Context {
    AnyExecutor executor;
    ClientConnection& connection;
    // Authentication token from a successful CreateSession; empty node id
    // before session activation.
    NodeId authentication_token;
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

  // Sends `request` and awaits the matching response. The returned body's
  // concrete type depends on the request; callers typically `std::get` or
  // `std::visit` on the variant.
  [[nodiscard]] Awaitable<StatusOr<ResponseBody>> Call(
      std::uint32_t request_handle,
      RequestBody request);

  // Lower-level split send/receive API for callers that keep multiple
  // requests outstanding (Publish). `Receive` buffers unrelated responses
  // for later matching by request_id.
  [[nodiscard]] Awaitable<StatusOr<std::uint32_t>> Send(
      std::uint32_t request_handle,
      RequestBody request);
  [[nodiscard]] Awaitable<StatusOr<ResponseBody>> Receive(
      std::uint32_t request_id,
      std::uint32_t request_handle);

 private:
  struct BufferedResponse {
    std::uint32_t request_handle = 0;
    ResponseBody body;
  };

  struct PendingResponse {
    explicit PendingResponse(AnyExecutor executor)
        : ready{std::move(executor)} {}

    std::uint32_t request_handle = 0;
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
  std::unordered_map<std::uint32_t, BufferedResponse> buffered_responses_;
  std::unordered_map<std::uint32_t, std::shared_ptr<PendingResponse>>
      pending_responses_;
  bool read_loop_running_ = false;
  bool login_complete_ = false;
  bool send_in_progress_ = false;
  std::deque<base::AsyncCompletion> send_waiters_;
};

}  // namespace opcua
