#include "opcua/session/server_runtime.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/async_completion.h"
#include "opcua/base/boost_log.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace opcua {

namespace {

BoostLogger logger_{LOG_NAME("ServerRuntime")};

template <typename Response>
Response SessionMissingResponse() {
  return {.status = StatusCode::Bad_SessionIdInvalid};
}

template <>
ResponseBody SessionMissingResponse<ResponseBody>() {
  return ServiceFault{StatusCode::Bad_SessionIdInvalid};
}

// Generated response types carry the service result in the embedded
// ResponseHeader rather than a top-level `status` field. request_handle is left
// 0 — the codec injects the envelope's handle at encode time.
template <>
ua::DeleteSubscriptionsResponse
SessionMissingResponse<ua::DeleteSubscriptionsResponse>() {
  ua::DeleteSubscriptionsResponse response;
  response.response_header.service_result =
      Status{StatusCode::Bad_SessionIdInvalid};
  return response;
}

template <>
ua::SetPublishingModeResponse
SessionMissingResponse<ua::SetPublishingModeResponse>() {
  ua::SetPublishingModeResponse response;
  response.response_header.service_result =
      Status{StatusCode::Bad_SessionIdInvalid};
  return response;
}

template <>
ua::DeleteMonitoredItemsResponse
SessionMissingResponse<ua::DeleteMonitoredItemsResponse>() {
  ua::DeleteMonitoredItemsResponse response;
  response.response_header.service_result =
      Status{StatusCode::Bad_SessionIdInvalid};
  return response;
}

template <>
ua::SetMonitoringModeResponse
SessionMissingResponse<ua::SetMonitoringModeResponse>() {
  ua::SetMonitoringModeResponse response;
  response.response_header.service_result =
      Status{StatusCode::Bad_SessionIdInvalid};
  return response;
}

template <>
ua::TransferSubscriptionsResponse
SessionMissingResponse<ua::TransferSubscriptionsResponse>() {
  ua::TransferSubscriptionsResponse response;
  response.response_header.service_result =
      Status{StatusCode::Bad_SessionIdInvalid};
  return response;
}

bool MatchesStringFilter(std::string_view value,
                         const std::vector<std::string>& filter) {
  return filter.empty() || std::ranges::find(filter, value) != filter.end();
}

}  // namespace

ServerRuntime::ServerRuntime(ServerRuntimeContext&& context)
    : executor_{std::move(context.executor)},
      session_manager_{context.session_manager},
      callbacks_{std::move(context.callbacks)},
      endpoints_{std::move(context.endpoints)},
      operation_limits_{context.operation_limits},
      now_{std::move(context.now)},
      post_delayed_task_{std::move(context.post_delayed_task)},
      register_server_{std::move(context.register_server)},
      registered_servers_{std::move(context.registered_servers)} {}

ServerRuntime::~ServerRuntime() = default;

Awaitable<ServiceResponse> ServerRuntime::HandleServiceRequest(
    const ServerSession& session,
    ServiceRequest request,
    const std::string& trace_parent) const {
  // A traceparent from the request header overrides the session context's
  // trace id for this one request, continuing the caller's trace.
  ServiceContext service_context = session.GetServiceContext();
  if (!trace_parent.empty()) {
    service_context = service_context.with_trace_id(trace_parent);
  }
  ServiceHandler handler{
      ServiceHandlerContext{.callbacks = callbacks_,
                            .service_context = std::move(service_context),
                            .operation_limits = operation_limits_}};
  co_return co_await handler.Handle(std::move(request));
}

void ServerRuntime::Detach(ConnectionState& connection) {
  if (!connection.authentication_token.has_value())
    return;

  LOG_INFO(logger_) << "OPC UA runtime detaching connection session"
                    << LOG_TAG("AuthenticationToken",
                               connection.authentication_token->ToString())
                    << LOG_TAG("Peer", connection.peer);
  session_manager_.DetachSession(*connection.authentication_token);
  connection.authentication_token.reset();
}

ServerSession* ServerRuntime::FindSession(
    const NodeId& authentication_token) const {
  const auto it = sessions_.find(authentication_token);
  return it != sessions_.end() ? it->second.get() : nullptr;
}

ServerSession* ServerRuntime::FindAttachedSession(
    const ConnectionState& connection) const {
  if (!connection.authentication_token.has_value())
    return nullptr;
  return FindSession(*connection.authentication_token);
}

void ServerRuntime::ForgetSession(const NodeId& authentication_token) {
  LOG_INFO(logger_) << "OPC UA runtime forgetting session state"
                    << LOG_TAG("AuthenticationToken",
                               authentication_token.ToString());
  RemoveSessionSubscriptions(authentication_token);
  sessions_.erase(authentication_token);
}

void ServerRuntime::RemoveSessionSubscriptions(
    const NodeId& authentication_token) {
  std::erase_if(subscription_owners_, [&](const auto& entry) {
    return entry.second == authentication_token;
  });
}

Awaitable<void> ServerRuntime::Delay(Duration delay) const {
  if (delay <= Duration{})
    co_return;

  base::AsyncCompletion delayed{executor_};
  auto callback = [delayed]() mutable { delayed.Complete(); };
  if (post_delayed_task_) {
    post_delayed_task_(delay, std::move(callback));
  } else {
    PostDelayedTask(executor_,
                    std::chrono::milliseconds{delay.InMilliseconds()},
                    std::move(callback));
  }
  co_await delayed.Wait();
}

Awaitable<ResponseBody> ServerRuntime::Handle(ConnectionState& connection,
                                              RequestBody request,
                                              std::string trace_parent) {
  auto body = co_await std::visit(
      [this, &connection,
       &trace_parent](auto&& typed_request) -> Awaitable<ResponseBody> {
        using T = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<T, FindServersRequest>) {
          co_return HandleFindServers(typed_request);
        } else if constexpr (std::is_same_v<T, GetEndpointsRequest>) {
          co_return HandleGetEndpoints(typed_request);
        } else if constexpr (std::is_same_v<T, RegisterServerRequest>) {
          co_return HandleRegisterServer(connection, typed_request);
        } else if constexpr (std::is_same_v<T, RegisterServer2Request>) {
          co_return HandleRegisterServer2(connection, typed_request);
        } else if constexpr (std::is_same_v<T, CreateSessionRequest>) {
          typed_request.channel_secure = connection.secure_channel;
          typed_request.channel_certificate = connection.client_certificate;
          typed_request.peer = connection.peer;
          co_return ResponseBody{co_await session_manager_.CreateSession(
              std::move(typed_request))};
        } else if constexpr (std::is_same_v<T, ActivateSessionRequest>) {
          co_return co_await HandleActivateSession(connection,
                                                   std::move(typed_request));
        } else if constexpr (std::is_same_v<T, CloseSessionRequest>) {
          const auto response = session_manager_.CloseSession(typed_request);
          if (response.status)
            ForgetSession(typed_request.authentication_token);
          if (connection.authentication_token ==
              typed_request.authentication_token)
            connection.authentication_token.reset();
          co_return ResponseBody{response};
        } else if constexpr (std::is_same_v<T, CreateSubscriptionRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<CreateSubscriptionResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          const auto response = session->CreateSubscriptionWithId(
              next_subscription_id_++, typed_request, trace_parent);
          subscription_owners_[response.subscription_id] =
              *connection.authentication_token;
          co_return ResponseBody{response};
        } else if constexpr (std::is_same_v<T, ModifySubscriptionRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<ModifySubscriptionResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->ModifySubscription(typed_request)};
        } else if constexpr (std::is_same_v<T, ua::SetPublishingModeRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<ua::SetPublishingModeResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->SetPublishingMode(typed_request)};
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteSubscriptionsRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<ua::DeleteSubscriptionsResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          const auto response = session->DeleteSubscriptions(typed_request);
          for (size_t i = 0; i < typed_request.subscription_ids.size(); ++i) {
            if (response.results[i].good())
              subscription_owners_.erase(typed_request.subscription_ids[i]);
          }
          co_return ResponseBody{response};
        } else if constexpr (std::is_same_v<T, PublishRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{SessionMissingResponse<PublishResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          auto ack_results = session->AcknowledgePublishRequest(typed_request);
          for (;;) {
            if (connection.closed) {
              co_return ResponseBody{SessionMissingResponse<PublishResponse>()};
            }

            session = FindAttachedSession(connection);
            if (!session) {
              co_return ResponseBody{SessionMissingResponse<PublishResponse>()};
            }

            // cppcheck-suppress nullPointerRedundantCheck
            auto poll = session->PollPublish();
            if (poll.response.has_value()) {
              poll.response->results = std::move(ack_results);
              co_return ResponseBody{std::move(*poll.response)};
            }
            if (!poll.wait_for.has_value()) {
              co_return ResponseBody{
                  PublishResponse{.status = StatusCode::Good,
                                  .results = std::move(ack_results)}};
            }
            co_await Delay(*poll.wait_for);
          }
        } else if constexpr (std::is_same_v<T, RepublishRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{SessionMissingResponse<RepublishResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->Republish(typed_request)};
        } else if constexpr (std::is_same_v<T,
                                            ua::TransferSubscriptionsRequest>) {
          auto* target_session = FindAttachedSession(connection);
          if (!target_session || !connection.authentication_token.has_value()) {
            co_return ResponseBody{
                SessionMissingResponse<ua::TransferSubscriptionsResponse>()};
          }

          ua::TransferSubscriptionsResponse response;
          response.results.assign(
              typed_request.subscription_ids.size(),
              ua::TransferResult{.status_code = Status{
                                     StatusCode::Bad_SubscriptionIdInvalid}});
          std::unordered_map<NodeId,
                             std::vector<std::pair<size_t, SubscriptionId>>>
              groups;

          for (size_t i = 0; i < typed_request.subscription_ids.size(); ++i) {
            const auto subscription_id = typed_request.subscription_ids[i];
            const auto owner_it = subscription_owners_.find(subscription_id);
            if (owner_it == subscription_owners_.end()) {
              response.results[i].status_code =
                  Status{StatusCode::Bad_SubscriptionIdInvalid};
              continue;
            }
            if (owner_it->second == *connection.authentication_token) {
              response.results[i].status_code = Status{StatusCode::Good};
              continue;
            }
            groups[owner_it->second].push_back({i, subscription_id});
          }

          for (const auto& [source_token, group] : groups) {
            auto* source_session = FindSession(source_token);
            if (!source_session) {
              for (const auto& [index, subscription_id] : group)
                response.results[index].status_code =
                    Status{StatusCode::Bad_SessionIdInvalid};
              continue;
            }

            ua::TransferSubscriptionsRequest grouped_request;
            grouped_request.send_initial_values =
                typed_request.send_initial_values;
            for (const auto& [index, subscription_id] : group)
              grouped_request.subscription_ids.push_back(subscription_id);

            const auto grouped_response =
                target_session->TransferSubscriptionsFrom(*source_session,
                                                          grouped_request);
            for (size_t i = 0; i < group.size(); ++i) {
              response.results[group[i].first] = grouped_response.results[i];
              if (grouped_response.results[i].status_code.good())
                subscription_owners_[group[i].second] =
                    *connection.authentication_token;
            }
          }

          co_return ResponseBody{response};
        } else if constexpr (std::is_same_v<T, CreateMonitoredItemsRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<CreateMonitoredItemsResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->CreateMonitoredItems(typed_request)};
        } else if constexpr (std::is_same_v<T, ModifyMonitoredItemsRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<ModifyMonitoredItemsResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->ModifyMonitoredItems(typed_request)};
        } else if constexpr (std::is_same_v<T,
                                            ua::DeleteMonitoredItemsRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<ua::DeleteMonitoredItemsResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->DeleteMonitoredItems(typed_request)};
        } else if constexpr (std::is_same_v<T, ua::SetMonitoringModeRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return ResponseBody{
                SessionMissingResponse<ua::SetMonitoringModeResponse>()};
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->SetMonitoringMode(typed_request)};
        } else if constexpr (std::is_same_v<T, ua::BrowseRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return SessionMissingResponse<ResponseBody>();
          // cppcheck-suppress nullPointerRedundantCheck
          auto& attached_session = *session;
          const auto requested_max_references_per_node =
              typed_request.requested_max_references_per_node;
          auto response = co_await HandleServiceRequest(
              attached_session, ServiceRequest{std::move(typed_request)},
              trace_parent);
          if (!std::holds_alternative<ua::BrowseResponse>(response))
            co_return SessionMissingResponse<ResponseBody>();
          auto browse = std::get<ua::BrowseResponse>(std::move(response));
          auto paged_response = session->StoreBrowseResults(
              std::move(browse), requested_max_references_per_node);
          co_return ResponseBody{std::move(paged_response)};
        } else if constexpr (std::is_same_v<T, ua::BrowseNextRequest>) {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return SessionMissingResponse<ResponseBody>();
          // cppcheck-suppress nullPointerRedundantCheck
          co_return ResponseBody{session->BrowseNext(typed_request)};
        } else if constexpr (std::is_same_v<T, ua::RegisterNodesRequest>) {
          // OPC UA Part 4 §5.3.2: registration is an optional optimization;
          // with no registered-node handles maintained, echo the requested
          // NodeIds.
          if (!FindAttachedSession(connection))
            co_return SessionMissingResponse<ResponseBody>();
          co_return ResponseBody{ua::RegisterNodesResponse{
              .registered_node_ids =
                  std::move(typed_request.nodes_to_register)}};
        } else if constexpr (std::is_same_v<T, ua::UnregisterNodesRequest>) {
          // OPC UA Part 4 §5.3.3: nothing to release, so this is a no-op.
          if (!FindAttachedSession(connection))
            co_return SessionMissingResponse<ResponseBody>();
          co_return ResponseBody{ua::UnregisterNodesResponse{}};
        } else {
          auto* session = FindAttachedSession(connection);
          if (!session)
            co_return SessionMissingResponse<ResponseBody>();
          assert(session != nullptr);
          auto service_response = co_await HandleServiceRequest(
              *session, ServiceRequest{std::move(typed_request)}, trace_parent);
          co_return std::visit(
              [](auto&& typed_response) -> ResponseBody {
                return ResponseBody{std::move(typed_response)};
              },
              std::move(service_response));
        }
      },
      std::move(request));
  co_return body;
}

ResponseBody ServerRuntime::HandleFindServers(
    const FindServersRequest& request) const {
  FindServersResponse response;
  const auto append = [&](const ApplicationDescription& server) {
    if (!MatchesStringFilter(server.application_uri, request.server_uris) &&
        !MatchesStringFilter(server.product_uri, request.server_uris)) {
      return;
    }
    const auto duplicate =
        std::ranges::find_if(response.servers, [&](const auto& existing) {
          return existing.application_uri == server.application_uri;
        }) != response.servers.end();
    if (!duplicate)
      response.servers.push_back(server);
  };

  // The server's own endpoints first, so they win the application_uri dedup.
  for (const auto& endpoint : endpoints_)
    append(endpoint.server);

  // Servers registered via RegisterServer (OPC UA Part 4 §5.4.2 FindServers,
  // discovery-server role): synthesized ApplicationDescriptions carrying the
  // registrant's identity and discovery URLs.
  if (registered_servers_) {
    for (const auto& registration : registered_servers_()) {
      append(ApplicationDescription{
          .application_uri = registration.server_uri,
          .product_uri = registration.product_uri,
          .application_name = registration.server_names.empty()
                                  ? LocalizedText{}
                                  : registration.server_names.front(),
          .application_type = registration.server_type,
          .discovery_urls = registration.discovery_urls});
    }
  }
  return ResponseBody{std::move(response)};
}

namespace {

// Scheme prefix of an OPC UA URL (e.g. "opc.tcp" from "opc.tcp://host:4840").
std::string_view UrlScheme(std::string_view url) {
  const auto pos = url.find("://");
  return pos == std::string_view::npos ? std::string_view{}
                                       : url.substr(0, pos);
}

}  // namespace

ResponseBody ServerRuntime::HandleGetEndpoints(
    const GetEndpointsRequest& request) const {
  GetEndpointsResponse response;
  for (auto endpoint : endpoints_) {
    if (!MatchesStringFilter(endpoint.transport_profile_uri,
                             request.profile_uris)) {
      continue;
    }
    // Return URLs reachable by the client: echo the URL the client used, but
    // only for endpoints served over the same transport scheme so a TCP
    // GetEndpoints does not clobber the advertised WS endpoint URLs (and vice
    // versa). OPC UA Part 4 §5.4.4 / Part 6 host normalization.
    if (!request.endpoint_url.empty() &&
        UrlScheme(endpoint.endpoint_url) == UrlScheme(request.endpoint_url)) {
      endpoint.endpoint_url = request.endpoint_url;
    }
    response.endpoints.push_back(std::move(endpoint));
  }
  return ResponseBody{std::move(response)};
}

ResponseBody ServerRuntime::HandleRegisterServer(
    const ConnectionState& connection,
    const RegisterServerRequest& request) const {
  // OPC UA Part 4 §5.4.5 RegisterServer. Delegated to the configured handler
  // (the aggregating proxy); a server with no handler is not a discovery
  // target. The handler receives the channel's security context so it can
  // reject registrations from untrusted callers.
  RegisterServerResponse response;
  response.status =
      register_server_
          ? register_server_(
                request.server,
                RegisterServerContext{
                    .channel_secure = connection.secure_channel,
                    .client_certificate = connection.client_certificate})
          : Status{StatusCode::Bad};
  return ResponseBody{std::move(response)};
}

ResponseBody ServerRuntime::HandleRegisterServer2(
    const ConnectionState& connection,
    const RegisterServer2Request& request) const {
  // OPC UA Part 4 §5.4.6 RegisterServer2,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.4.6 — same
  // delegation as RegisterServer, plus the discoveryConfiguration whose
  // ServerCapabilityIdentifiers reach the handler through the context. Per
  // §5.4.6.2, configurationResults carries one status per
  // discoveryConfiguration entry: Good for the MdnsDiscoveryConfiguration
  // entries this stack decodes, Bad_NotSupported for unknown extension types.
  RegisterServer2Response response;
  if (!register_server_) {
    response.status = Status{StatusCode::Bad};
    return ResponseBody{std::move(response)};
  }
  RegisterServerContext context{
      .channel_secure = connection.secure_channel,
      .client_certificate = connection.client_certificate};
  response.configuration_results.reserve(
      request.discovery_configuration.size());
  for (const auto& configuration : request.discovery_configuration) {
    if (configuration.has_value()) {
      context.server_capabilities.insert(
          context.server_capabilities.end(),
          configuration->server_capabilities.begin(),
          configuration->server_capabilities.end());
      response.configuration_results.push_back(StatusCode::Good);
    } else {
      response.configuration_results.push_back(StatusCode::Bad_NotSupported);
    }
  }
  response.status = register_server_(request.server, std::move(context));
  return ResponseBody{std::move(response)};
}

Awaitable<ResponseBody> ServerRuntime::HandleActivateSession(
    ConnectionState& connection,
    ActivateSessionRequest request) {
  request.channel_secure = connection.secure_channel;
  request.peer = connection.peer;
  const auto response = co_await session_manager_.ActivateSession(request);
  if (!response.status)
    co_return ResponseBody{response};

  std::shared_ptr<ServerSession> session;
  if (response.resumed) {
    auto* attached_session = FindSession(request.authentication_token);
    session =
        attached_session ? sessions_.at(request.authentication_token) : nullptr;
    if (!session) {
      co_return ResponseBody{
          ActivateSessionResponse{.status = StatusCode::Bad_SessionIdInvalid}};
    }
    // The resumed session may now be served over a different connection; adopt
    // the refreshed context (same identity, current peer) for request logs.
    session->SetServiceContext(response.service_context);
  } else {
    session = std::make_shared<ServerSession>(ServerSessionContext{
        .session_id = request.session_id,
        .authentication_token = request.authentication_token,
        .service_context = response.service_context,
        .executor = executor_,
        .create_subscription = callbacks_.create_subscription,
        .operation_limits = operation_limits_,
        .now = now_,
    });
    sessions_[request.authentication_token] = session;
  }

  connection.authentication_token = request.authentication_token;
  co_return ResponseBody{response};
}

}  // namespace opcua
