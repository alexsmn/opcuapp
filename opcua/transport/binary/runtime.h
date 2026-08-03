#pragma once

#include "opcua/base/any_executor.h"

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/session/server_runtime.h"
#include "opcua/transport/binary/service_codec.h"

namespace opcua::binary {

template <typename Response>
Response BuildRuntimeErrorResponse(Status status) {
  if constexpr (requires(Response response) { response.status; }) {
    return Response{.status = std::move(status)};
  } else if constexpr (requires(Response response) {
                         response.response_header.service_result;
                       }) {
    // The generated ua:: response types carry the service-level status in
    // their ResponseHeader rather than in a `status` member. Without this
    // branch they matched none of the others — `results` is a vector of
    // DataValue/StatusCode-shaped elements with no `status` member — and fell
    // through to `return Response{}`, so a rejected request was answered with
    // a default-constructed response whose service_result is Good. On the
    // authenticated-request path that turned a Bad_SessionIdInvalid refusal
    // into an apparently successful empty read.
    Response response;
    response.response_header.service_result = std::move(status);
    return response;
  } else if constexpr (requires(Response response) {
                         response.result.status;
                       }) {
    auto response = Response{};
    response.result.status = std::move(status);
    return response;
  } else if constexpr (requires(Response response) { response.results; }) {
    using Result = typename decltype(Response{}.results)::value_type;
    if constexpr (requires(Result result) { result.status; }) {
      return Response{.results = {Result{.status = std::move(status)}}};
    } else {
      return Response{};
    }
  } else {
    return Response{};
  }
}

using ConnectionState = opcua::ConnectionState;

struct RuntimeContext {
  AnyExecutor executor;
  ServerSessionManager& session_manager;
  ServiceCallbacks callbacks;
  std::vector<EndpointDescription> endpoints;
  // Forwarded to the wrapped ServerRuntime. Both were previously absent here,
  // so the UA Binary transport silently ran on ServerRuntimeContext's defaults:
  // an embedder that configured operation limits had them enforced on every
  // other path while Binary — the primary production transport — ignored them,
  // against `OperationLimits`' own requirement that one struct instance drive
  // both exposure and enforcement.
  OperationLimits operation_limits;
  std::function<DateTime()> now = &DateTime::Now;
  // Optional override for delayed task scheduling. Defaults to
  // boost::asio::steady_timer-based posting when null. Tests substitute a
  // capturing scheduler; without it a held Publish never completes on an
  // executor that has no timer reactor.
  std::function<void(Duration, std::function<void()>)> post_delayed_task;
  // Optional RegisterServer handler (the aggregating proxy registers
  // downstreams). Receives the channel security context so it can reject
  // untrusted callers.
  std::function<Status(const RegisteredServer&, const RegisterServerContext&)>
      register_server;
  // Optional snapshot of servers registered via RegisterServer, surfaced
  // through FindServers (see ServerRuntimeContext::registered_servers).
  std::function<std::vector<RegisteredServer>()> registered_servers;
};

// UA Binary reuses the canonical shared server-side session/subscription/
// service runtime while keeping Binary-specific framing and codec logic local.
class Runtime {
 public:
  explicit Runtime(RuntimeContext&& context);

  // `trace_parent` carries the W3C traceparent from the decoded request
  // header (empty = absent); see ServiceRequestHeader::trace_parent.
  template <typename Response, typename Request>
  [[nodiscard]] Awaitable<Response> Handle(ConnectionState& connection,
                                           Request request,
                                           std::string trace_parent = {}) {
    auto body = co_await HandleBody(connection, RequestBody{std::move(request)},
                                    std::move(trace_parent));
    if (auto* typed = std::get_if<Response>(&body)) {
      co_return std::move(*typed);
    }
    if (auto* fault = std::get_if<ServiceFault>(&body)) {
      co_return BuildRuntimeErrorResponse<Response>(fault->status);
    }
    co_return BuildRuntimeErrorResponse<Response>(StatusCode::Bad);
  }

  void Detach(ConnectionState& connection);

  [[nodiscard]] Awaitable<std::optional<ResponseBody>> HandleDecodedRequest(
      ConnectionState& connection,
      const DecodedRequest& request);

 private:
  [[nodiscard]] Awaitable<ResponseBody> HandleBody(
      ConnectionState& connection,
      RequestBody request,
      std::string trace_parent = {});

  template <typename Response, typename Request>
  [[nodiscard]] Awaitable<std::optional<ResponseBody>>
  HandleAuthenticatedRequest(ConnectionState& connection,
                             const DecodedRequest& request,
                             Request typed_request) {
    if (!connection.authentication_token.has_value() ||
        *connection.authentication_token !=
            request.header.authentication_token) {
      co_return ResponseBody{BuildRuntimeErrorResponse<Response>(
          StatusCode::Bad_SessionIdInvalid)};
    }

    co_return ResponseBody{co_await Handle<Response>(
        connection, std::move(typed_request), request.header.trace_parent)};
  }

  [[nodiscard]] Awaitable<std::optional<ResponseBody>> HandleSessionRequest(
      ConnectionState& connection,
      CreateSessionRequest request);
  [[nodiscard]] Awaitable<std::optional<ResponseBody>> HandleSessionRequest(
      ConnectionState& connection,
      const ServiceRequestHeader& header,
      ActivateSessionRequest request);
  [[nodiscard]] Awaitable<std::optional<ResponseBody>> HandleSessionRequest(
      ConnectionState& connection,
      const ServiceRequestHeader& header,
      CloseSessionRequest request);

  ServerSessionManager& session_manager_;
  ServerRuntime runtime_;
};

}  // namespace opcua::binary
