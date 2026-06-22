#include "opcua/transport/binary/runtime.h"

namespace opcua::binary {
namespace {
template <typename T>
constexpr bool kIsSessionRequest = std::is_same_v<T, CreateSessionRequest> ||
                                   std::is_same_v<T, ActivateSessionRequest> ||
                                   std::is_same_v<T, CloseSessionRequest>;

template <typename Request>
struct AuthenticatedRequestTraits;

#define OPCUA_BINARY_AUTHENTICATED_REQUESTS(X)                   \
  X(BrowseNextRequest, BrowseNextResponse)                       \
  X(ReadRequest, ReadResponse)                                   \
  X(BrowseRequest, BrowseResponse)                               \
  X(TranslateBrowsePathsRequest, TranslateBrowsePathsResponse)   \
  X(CallRequest, CallResponse)                                   \
  X(HistoryReadRawRequest, HistoryReadRawResponse)               \
  X(HistoryReadEventsRequest, HistoryReadEventsResponse)         \
  X(HistoryUpdateRequest, HistoryUpdateResponse)                 \
  X(WriteRequest, WriteResponse)                                 \
  X(DeleteNodesRequest, DeleteNodesResponse)                     \
  X(AddNodesRequest, AddNodesResponse)                           \
  X(DeleteReferencesRequest, DeleteReferencesResponse)           \
  X(AddReferencesRequest, AddReferencesResponse)                 \
  X(CreateSubscriptionRequest, CreateSubscriptionResponse)       \
  X(ModifySubscriptionRequest, ModifySubscriptionResponse)       \
  X(SetPublishingModeRequest, SetPublishingModeResponse)         \
  X(DeleteSubscriptionsRequest, DeleteSubscriptionsResponse)     \
  X(CreateMonitoredItemsRequest, CreateMonitoredItemsResponse)   \
  X(ModifyMonitoredItemsRequest, ModifyMonitoredItemsResponse)   \
  X(PublishRequest, PublishResponse)                             \
  X(RepublishRequest, RepublishResponse)                         \
  X(TransferSubscriptionsRequest, TransferSubscriptionsResponse) \
  X(DeleteMonitoredItemsRequest, DeleteMonitoredItemsResponse)   \
  X(SetMonitoringModeRequest, SetMonitoringModeResponse)

#define OPCUA_BINARY_DECLARE_AUTH_TRAITS(Request, Response) \
  template <>                                               \
  struct AuthenticatedRequestTraits<Request> {              \
    using ResponseType = Response;                          \
  };
OPCUA_BINARY_AUTHENTICATED_REQUESTS(OPCUA_BINARY_DECLARE_AUTH_TRAITS)
#undef OPCUA_BINARY_DECLARE_AUTH_TRAITS

template <typename Request>
using AuthenticatedResponse =
    typename AuthenticatedRequestTraits<Request>::ResponseType;

template <typename Request>
concept AuthenticatedRequest =
    requires { typename AuthenticatedResponse<Request>; };

#undef OPCUA_BINARY_AUTHENTICATED_REQUESTS
}  // namespace

Runtime::Runtime(RuntimeContext&& context)
    : session_manager_{context.session_manager},
      runtime_{ServerRuntimeContext{
          .executor = context.executor,
          .session_manager = context.session_manager,
          .callbacks = std::move(context.callbacks),
          .endpoints = std::move(context.endpoints),
          .now = std::move(context.now),
      }} {}

Awaitable<ResponseBody> Runtime::HandleBody(ConnectionState& connection,
                                            RequestBody request) {
  co_return co_await runtime_.Handle(connection, std::move(request));
}

void Runtime::Detach(ConnectionState& connection) {
  runtime_.Detach(connection);
}

Awaitable<std::optional<ResponseBody>> Runtime::HandleSessionRequest(
    ConnectionState& connection,
    CreateSessionRequest request) {
  co_return ResponseBody{
      co_await Handle<CreateSessionResponse>(connection, std::move(request))};
}

Awaitable<std::optional<ResponseBody>> Runtime::HandleSessionRequest(
    ConnectionState& connection,
    const ServiceRequestHeader& header,
    ActivateSessionRequest request) {
  const auto session =
      session_manager_.FindSession(header.authentication_token);
  if (!session.has_value()) {
    co_return std::nullopt;
  }

  request.session_id = session->session_id;
  request.authentication_token = header.authentication_token;
  co_return ResponseBody{
      co_await Handle<ActivateSessionResponse>(connection, std::move(request))};
}

Awaitable<std::optional<ResponseBody>> Runtime::HandleSessionRequest(
    ConnectionState& connection,
    const ServiceRequestHeader& header,
    CloseSessionRequest request) {
  const auto session =
      session_manager_.FindSession(header.authentication_token);
  if (!session.has_value()) {
    co_return ResponseBody{
        CloseSessionResponse{.status = StatusCode::Bad_SessionIsLoggedOff}};
  }

  request.session_id = session->session_id;
  request.authentication_token = header.authentication_token;
  co_return ResponseBody{
      co_await Handle<CloseSessionResponse>(connection, std::move(request))};
}

Awaitable<std::optional<ResponseBody>> Runtime::HandleDecodedRequest(
    ConnectionState& connection,
    const DecodedRequest& request) {
  co_return co_await std::visit(
      [this, &connection,
       &request](auto typed_request) -> Awaitable<std::optional<ResponseBody>> {
        using T = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<T, FindServersRequest> ||
                      std::is_same_v<T, GetEndpointsRequest>) {
          co_return co_await HandleBody(connection,
                                        RequestBody{std::move(typed_request)});
        } else if constexpr (std::is_same_v<T, CreateSessionRequest>) {
          co_return co_await HandleSessionRequest(connection,
                                                  std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ActivateSessionRequest>) {
          co_return co_await HandleSessionRequest(connection, request.header,
                                                  std::move(typed_request));
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
          co_return co_await HandleSessionRequest(connection, request.header,
                                                  std::move(typed_request));
        } else if constexpr (AuthenticatedRequest<T>) {
          co_return co_await HandleAuthenticatedRequest<
              AuthenticatedResponse<T>>(connection, request,
                                        std::move(typed_request));
        } else {
          static_assert(!kIsSessionRequest<T>,
                        "Session requests must be handled above");
          co_return std::nullopt;
        }
      },
      request.body);
}

}  // namespace opcua::binary
