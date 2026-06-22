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
  std::function<base::Time()> now = &base::Time::Now;
};

// UA Binary reuses the canonical shared server-side session/subscription/
// service runtime while keeping Binary-specific framing and codec logic local.
class Runtime {
 public:
  explicit Runtime(RuntimeContext&& context);

  template <typename Response, typename Request>
  [[nodiscard]] Awaitable<Response> Handle(ConnectionState& connection,
                                           Request request) {
    auto body =
        co_await HandleBody(connection, RequestBody{std::move(request)});
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
  [[nodiscard]] Awaitable<ResponseBody> HandleBody(ConnectionState& connection,
                                                   RequestBody request);

  template <typename Response, typename Request>
  [[nodiscard]] Awaitable<std::optional<ResponseBody>>
  HandleAuthenticatedRequest(ConnectionState& connection,
                             const DecodedRequest& request,
                             Request typed_request) {
    if (!connection.authentication_token.has_value() ||
        *connection.authentication_token !=
            request.header.authentication_token) {
      co_return ResponseBody{BuildRuntimeErrorResponse<Response>(
          StatusCode::Bad_SessionIsLoggedOff)};
    }

    co_return ResponseBody{
        co_await Handle<Response>(connection, std::move(typed_request))};
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
