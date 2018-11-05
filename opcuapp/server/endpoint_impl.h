#pragma once

#include <opcua_core.h>
#include <opcua_endpoint.h>
#include <opcua_servicetable.h>
#include <opcuapp/basic_types.h>
#include <opcuapp/node_id.h>
#include <opcuapp/requests.h>
#include <opcuapp/server/endpoint.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/server/session.h>
#include <opcuapp/status_code.h>
#include <opcuapp/structs.h>
#include <opcuapp/vector.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace opcua {
namespace server {

class Session;

namespace detail {

class EndpointImpl : public std::enable_shared_from_this<EndpointImpl> {
 public:
  explicit EndpointImpl(OpcUa_Endpoint_SerializerType serializer_type);
  ~EndpointImpl();

  OpcUa_Handle handle() const { return handle_; }
  const String& url() const { return url_; }

  void set_application_uri(String uri) { application_uri_ = std::move(uri); }

  void set_product_uri(String uri) { product_uri_ = std::move(uri); }

  void set_application_name(LocalizedText name) {
    application_name_ = std::move(name);
  }

  void set_status_handler(Endpoint::StatusHandler handler) {
    status_handler_ = std::move(handler);
  }
  void set_session_handlers(SessionHandlers handlers) {
    session_handlers_ = std::move(handlers);
  }

  // WARNING: Referenced parameters must outlive the Endpoint.
  void Open(
      String url,
      bool listen_on_all_interfaces,
      const OpcUa_ByteString& server_certificate,
      const OpcUa_Key& server_private_key,
      const OpcUa_Void* pki_config,
      Span<const Endpoint::SecurityPolicyConfiguration> security_policies);

  void Close();

 private:
  ApplicationDescription GetApplicationDescription() const;
  EndpointDescription GetEndpointDescription() const;

  NodeId MakeAuthenticationToken();
  NodeId MakeSessionId();

  std::shared_ptr<Session> CreateSession(String session_name);
  std::shared_ptr<Session> GetSession(const NodeId& authentication_token);

  std::vector<const OpcUa_ServiceType*> MakeSupportedServices() const;

  void BeginInvoke(
      OpcUa_GetEndpointsRequest& request,
      const std::function<void(GetEndpointsResponse& response)>& callback);
  void BeginInvoke(
      OpcUa_FindServersRequest& request,
      const std::function<void(FindServersResponse& response)>& callback);
  void BeginInvoke(
      OpcUa_CreateSessionRequest& request,
      const std::function<void(CreateSessionResponse& response)>& callback);

  // WARNING: Supresses errors.
  template <class Response>
  void SendResponse(OpcUa_Handle& context,
                    const OpcUa_RequestHeader& request_header,
                    Response&& response);

  // WARNING: Suppresses errors.
  void SendFault(OpcUa_Handle& context,
                 const OpcUa_RequestHeader& request_header,
                 ResponseHeader&& response_header);

  template <class Request, class Response>
  static OpcUa_StatusCode BeginInvokeEndpoint(
      OpcUa_Endpoint a_hEndpoint,
      OpcUa_Handle a_hContext,
      OpcUa_Void** a_ppRequest,
      OpcUa_EncodeableType* a_pRequestType);

  template <class Request, class Response>
  static OpcUa_StatusCode BeginInvokeSession(
      OpcUa_Endpoint a_hEndpoint,
      OpcUa_Handle a_hContext,
      OpcUa_Void** a_ppRequest,
      OpcUa_EncodeableType* a_pRequestType);

  template <class Request, class Response>
  static OpcUa_StatusCode BeginInvokeSubscription(
      OpcUa_Endpoint a_hEndpoint,
      OpcUa_Handle a_hContext,
      OpcUa_Void** a_ppRequest,
      OpcUa_EncodeableType* a_pRequestType);

  static opcua::Vector<OpcUa_EndpointDescription> GetEndpoints();
  static opcua::Vector<OpcUa_ApplicationDescription> FindServers();

  static OpcUa_StatusCode Invoke(OpcUa_Endpoint hEndpoint,
                                 OpcUa_Void* pvCallbackData,
                                 OpcUa_Endpoint_Event eEvent,
                                 OpcUa_StatusCode uStatus,
                                 OpcUa_UInt32 uSecureChannelId,
                                 OpcUa_ByteString* pbsClientCertificate,
                                 OpcUa_String* pSecurityPolicy,
                                 OpcUa_UInt16 uSecurityMode);

  struct Registry {
    std::mutex mutex;
    std::map<OpcUa_Handle /*endpoint*/, std::shared_ptr<EndpointImpl>>
        endpoints;
  };

  static Registry& registry();

  static void AddEndpoint(OpcUa_Handle endpoint_handle,
                          std::shared_ptr<EndpointImpl> endpoint);
  static void RemoveEndpoint(OpcUa_Handle endpoint_handle);
  static std::shared_ptr<EndpointImpl> GetEndpoint(
      OpcUa_Handle endpoint_handle);

  String url_;
  String application_uri_;
  String product_uri_;
  LocalizedText application_name_;

  Endpoint::StatusHandler status_handler_;
  SessionHandlers session_handlers_;

  OpcUa_Endpoint handle_ = OpcUa_Null;

  std::mutex mutex_;

  using Sessions =
      std::map<NodeId /*authentication_token*/, std::shared_ptr<Session>>;
  Sessions sessions_;

  unsigned next_session_id_ = 1;
};

inline EndpointImpl::EndpointImpl(
    OpcUa_Endpoint_SerializerType serializer_type) {
  Check(::OpcUa_Endpoint_Create(
      &handle_, serializer_type,
      const_cast<OpcUa_ServiceType**>(MakeSupportedServices().data())));
}

inline EndpointImpl::~EndpointImpl() {
  ::OpcUa_Endpoint_Delete(&handle_);
}

inline void EndpointImpl::Close() {
  ::OpcUa_Endpoint_Close(handle_);

  Sessions sessions;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    sessions = std::move(sessions_);
  }

  for (auto& p : sessions)
    p.second->Close();

  RemoveEndpoint(handle_);
}

// static
inline OpcUa_StatusCode EndpointImpl::Invoke(
    OpcUa_Endpoint hEndpoint,
    OpcUa_Void* pvCallbackData,
    OpcUa_Endpoint_Event eEvent,
    OpcUa_StatusCode uStatus,
    OpcUa_UInt32 uSecureChannelId,
    OpcUa_ByteString* pbsClientCertificate,
    OpcUa_String* pSecurityPolicy,
    OpcUa_UInt16 uSecurityMode) {
  auto& impl = *static_cast<EndpointImpl*>(pvCallbackData);
  if (impl.status_handler_)
    impl.status_handler_(eEvent);
  return OpcUa_Good;
}

inline void EndpointImpl::Open(
    String url,
    bool listen_on_all_interfaces,
    const OpcUa_ByteString& server_certificate,
    const OpcUa_Key& server_private_key,
    const OpcUa_Void* pki_config,
    Span<const Endpoint::SecurityPolicyConfiguration> security_policies) {
  AddEndpoint(handle_, shared_from_this());

  url_ = std::move(url);

  Check(::OpcUa_Endpoint_Open(
      handle_, url_.raw_string(),
      listen_on_all_interfaces ? OpcUa_True : OpcUa_False,
      &EndpointImpl::Invoke, this,
      &const_cast<OpcUa_ByteString&>(server_certificate),
      &const_cast<OpcUa_Key&>(server_private_key),
      const_cast<OpcUa_Void*>(pki_config), security_policies.size(),
      const_cast<Endpoint::SecurityPolicyConfiguration*>(
          security_policies.data())));
}

inline std::vector<const OpcUa_ServiceType*>
EndpointImpl::MakeSupportedServices() const {
  static const OpcUa_ServiceType kServiceTypes[] = {
      {
          OpcUaId_GetEndpointsRequest,
          &OpcUa_GetEndpointsResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeEndpoint<OpcUa_GetEndpointsRequest,
                                   GetEndpointsResponse>),
      },
      {
          OpcUaId_FindServersRequest,
          &OpcUa_FindServersResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeEndpoint<OpcUa_FindServersRequest,
                                   FindServersResponse>),
      },
      {
          OpcUaId_CreateSessionRequest,
          &OpcUa_CreateSessionResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeEndpoint<OpcUa_CreateSessionRequest,
                                   CreateSessionResponse>),
      },
      {
          OpcUaId_ActivateSessionRequest,
          &OpcUa_ActivateSessionResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_ActivateSessionRequest,
                                  ActivateSessionResponse>),
      },
      {
          OpcUaId_CloseSessionRequest,
          &OpcUa_CloseSessionResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_CloseSessionRequest,
                                  CloseSessionResponse>),
      },
      {
          OpcUaId_ReadRequest,
          &OpcUa_ReadResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_ReadRequest, ReadResponse>),
      },
      {
          OpcUaId_BrowseRequest,
          &OpcUa_BrowseResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_BrowseRequest, BrowseResponse>),
      },
      {
          OpcUaId_TranslateBrowsePathsToNodeIdsRequest,
          &OpcUa_BrowseResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_TranslateBrowsePathsToNodeIdsRequest,
                                  TranslateBrowsePathsToNodeIdsResponse>),
      },
      {
          OpcUaId_CreateSubscriptionRequest,
          &OpcUa_CreateSubscriptionResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_CreateSubscriptionRequest,
                                  CreateSubscriptionResponse>),
      },
      {
          OpcUaId_CreateMonitoredItemsRequest,
          &OpcUa_CreateMonitoredItemsResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSubscription<OpcUa_CreateMonitoredItemsRequest,
                                       CreateMonitoredItemsResponse>),
      },
      {
          OpcUaId_DeleteMonitoredItemsRequest,
          &OpcUa_DeleteMonitoredItemsResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSubscription<OpcUa_DeleteMonitoredItemsRequest,
                                       DeleteMonitoredItemsResponse>),
      },
      {
          OpcUaId_PublishRequest,
          &OpcUa_PublishResponse_EncodeableType,
          static_cast<OpcUa_PfnBeginInvokeService*>(
              &BeginInvokeSession<OpcUa_PublishRequest, PublishResponse>),
      },
  };

  std::vector<const OpcUa_ServiceType*> result(std::size(kServiceTypes) + 1,
                                               OpcUa_Null);
  std::transform(std::begin(kServiceTypes), std::end(kServiceTypes),
                 result.begin(), [](auto& v) { return &v; });
  return result;
}

template <class Request, class Response>
inline OpcUa_StatusCode EndpointImpl::BeginInvokeEndpoint(
    OpcUa_Endpoint a_hEndpoint,
    OpcUa_Handle a_hContext,
    OpcUa_Void** a_ppRequest,
    OpcUa_EncodeableType* a_pRequestType) {
  auto& request = *reinterpret_cast<Request*>(*a_ppRequest);

  auto endpoint = GetEndpoint(a_hEndpoint);
  if (!endpoint) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    endpoint->SendFault(a_hContext, request.RequestHeader,
                        std::move(response_header));
    return OpcUa_Good;
  }

  endpoint->BeginInvoke(request, [endpoint, a_hContext,
                                  request_header = request.RequestHeader](
                                     Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      endpoint->SendResponse(a_hContext, request_header, std::move(response));
    else
      endpoint->SendFault(a_hContext, request_header,
                          std::move(response.ResponseHeader));
  });

  return OpcUa_Good;
}

template <class Request, class Response>
inline OpcUa_StatusCode EndpointImpl::BeginInvokeSession(
    OpcUa_Endpoint a_hEndpoint,
    OpcUa_Handle a_hContext,
    OpcUa_Void** a_ppRequest,
    OpcUa_EncodeableType* a_pRequestType) {
  auto& request = *reinterpret_cast<Request*>(*a_ppRequest);

  auto endpoint = GetEndpoint(a_hEndpoint);
  if (!endpoint) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    endpoint->SendFault(a_hContext, request.RequestHeader,
                        std::move(response_header));
    return OpcUa_Good;
  }

  auto session =
      endpoint->GetSession(request.RequestHeader.AuthenticationToken);
  if (!session) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    endpoint->SendFault(a_hContext, request.RequestHeader,
                        std::move(response_header));
    return OpcUa_Good;
  }

  session->BeginInvoke(request, [endpoint, a_hContext,
                                 request_header = request.RequestHeader](
                                    Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      endpoint->SendResponse(a_hContext, request_header, std::move(response));
    else
      endpoint->SendFault(a_hContext, request_header,
                          std::move(response.ResponseHeader));
  });

  return OpcUa_Good;
}

template <class Request, class Response>
inline OpcUa_StatusCode EndpointImpl::BeginInvokeSubscription(
    OpcUa_Endpoint a_hEndpoint,
    OpcUa_Handle a_hContext,
    OpcUa_Void** a_ppRequest,
    OpcUa_EncodeableType* a_pRequestType) {
  auto& request = *reinterpret_cast<Request*>(*a_ppRequest);

  auto endpoint = GetEndpoint(a_hEndpoint);
  if (!endpoint) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    endpoint->SendFault(a_hContext, request.RequestHeader,
                        std::move(response_header));
    return OpcUa_Good;
  }

  auto session =
      endpoint->GetSession(request.RequestHeader.AuthenticationToken);
  auto subscription =
      session ? session->GetSubscription(request.SubscriptionId) : nullptr;
  if (!subscription) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    endpoint->SendFault(a_hContext, request.RequestHeader,
                        std::move(response_header));
    return OpcUa_Good;
  }

  subscription->BeginInvoke(request, [endpoint, a_hContext,
                                      request_header = request.RequestHeader](
                                         Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      endpoint->SendResponse(a_hContext, request_header, std::move(response));
    else
      endpoint->SendFault(a_hContext, request_header,
                          std::move(response.ResponseHeader));
  });

  return OpcUa_Good;
}

inline void PrepareResponse(const OpcUa_RequestHeader& request_header,
                            OpcUa_StatusCode status_code,
                            OpcUa_ResponseHeader& response_header) {
  ::OpcUa_ResponseHeader_Initialize(&response_header);
  response_header.RequestHandle = request_header.RequestHandle;

  /*auto duration = GetDateTimeDiff(request_header.Timestamp,
  ::OpcUa_DateTime_UtcNow()); if (duration > std::chrono::seconds(1))
    response_header.ServiceResult = OpcUa_BadTimeout;
  else
    response_header.ServiceResult = status_code;*/

  response_header.ServiceResult = status_code;
  response_header.Timestamp = ::OpcUa_DateTime_UtcNow();

  if (request_header.ReturnDiagnostics) {
    response_header.ServiceDiagnostics.SymbolicId = -1;
    response_header.ServiceDiagnostics.NamespaceUri = -1;
    response_header.ServiceDiagnostics.LocalizedText = -1;
    response_header.ServiceDiagnostics.Locale = -1;
  }
}

// static
inline void EndpointImpl::BeginInvoke(
    OpcUa_GetEndpointsRequest& request,
    const std::function<void(GetEndpointsResponse& response)>& callback) {
  Span<const OpcUa_String> profile_uris{
      request.ProfileUris, static_cast<size_t>(request.NoOfProfileUris)};

  GetEndpointsResponse response;

  if (!profile_uris.empty()) {
    PrepareResponse(request.RequestHeader, OpcUa_BadNotImplemented,
                    response.ResponseHeader);
    callback(response);
    return;
  }

  PrepareResponse(request.RequestHeader, OpcUa_Good, response.ResponseHeader);
  auto endpoints = GetEndpoints();
  response.NoOfEndpoints = endpoints.size();
  response.Endpoints = endpoints.release();
  callback(response);
}

// static
inline Vector<OpcUa_EndpointDescription> EndpointImpl::GetEndpoints() {
  std::lock_guard<std::mutex> lock{registry().mutex};
  Vector<OpcUa_EndpointDescription> endpoints(registry().endpoints.size());
  size_t i = 0;
  for (auto& p : registry().endpoints)
    p.second->GetEndpointDescription().release(endpoints[i++]);
  return endpoints;
}

// static
inline void EndpointImpl::BeginInvoke(
    OpcUa_FindServersRequest& request,
    const std::function<void(FindServersResponse& response)>& callback) {
  Span<const OpcUa_String> server_uris{
      request.ServerUris, static_cast<size_t>(request.NoOfServerUris)};
  Span<const OpcUa_String> locale_ids{
      request.LocaleIds, static_cast<size_t>(request.NoOfLocaleIds)};

  FindServersResponse response;

  if (!server_uris.empty()) {
    PrepareResponse(request.RequestHeader, OpcUa_BadNotImplemented,
                    response.ResponseHeader);
    callback(response);
    return;
  }

  auto servers = FindServers();
  response.NoOfServers = servers.size();
  response.Servers = servers.release();
  PrepareResponse(request.RequestHeader, OpcUa_Good, response.ResponseHeader);
  callback(response);
}

// static
inline Vector<OpcUa_ApplicationDescription> EndpointImpl::FindServers() {
  std::lock_guard<std::mutex> lock{registry().mutex};
  Vector<OpcUa_ApplicationDescription> servers(registry().endpoints.size());
  size_t index = 0;
  for (auto& p : registry().endpoints)
    p.second->GetApplicationDescription().release(servers[index++]);
  return servers;
}

inline NodeId EndpointImpl::MakeAuthenticationToken() {
  // TODO: Namespace index.
  return NodeId{"FixMe", 101};
}

inline NodeId EndpointImpl::MakeSessionId() {
  // TODO: Namespace index.
  return NodeId{next_session_id_++, 100};
}

// static
inline void EndpointImpl::BeginInvoke(
    OpcUa_CreateSessionRequest& request,
    const std::function<void(CreateSessionResponse& response)>& callback) {
  auto session = CreateSession(std::move(request.SessionName));

  CreateSessionResponse response;
  response.RevisedSessionTimeout = request.RequestedSessionTimeout;
  session->id().CopyTo(response.SessionId);
  session->authentication_token().CopyTo(response.AuthenticationToken);
  response.MaxRequestMessageSize = request.MaxResponseMessageSize;
  auto endpoints = GetEndpoints();
  response.NoOfServerEndpoints = endpoints.size();
  response.ServerEndpoints = endpoints.release();
  PrepareResponse(request.RequestHeader, OpcUa_Good, response.ResponseHeader);
  callback(response);
}

template <class Response>
inline void EndpointImpl::SendResponse(
    OpcUa_Handle& context,
    const OpcUa_RequestHeader& request_header,
    Response&& response) {
  PrepareResponse(request_header, response.ResponseHeader.ServiceResult,
                  response.ResponseHeader);
  ::OpcUa_Endpoint_EndSendResponse(
      handle_, &context, OpcUa_Good, &response,
      const_cast<OpcUa_EncodeableType*>(&Response::type()));
}

inline void EndpointImpl::SendFault(OpcUa_Handle& context,
                                    const OpcUa_RequestHeader& request_header,
                                    ResponseHeader&& response_header) {
  OpcUa_Void* response = OpcUa_Null;
  OpcUa_EncodeableType* response_type = OpcUa_Null;
  if (OpcUa_IsBad(::OpcUa_ServerApi_CreateFault(
          const_cast<OpcUa_RequestHeader*>(&request_header),
          response_header.ServiceResult, &response_header.ServiceDiagnostics,
          &response_header.NoOfStringTable, &response_header.StringTable,
          &response, &response_type))) {
    return;
  }

  ::OpcUa_Endpoint_EndSendResponse(handle_, &context, OpcUa_Good, response,
                                   response_type);
  ::OpcUa_EncodeableObject_Delete(response_type, &response);
}

inline ApplicationDescription EndpointImpl::GetApplicationDescription() const {
  ApplicationDescription result;
  ::OpcUa_String_AttachCopy(&result.ApplicationUri,
                            application_uri_.raw_string());
  ::OpcUa_String_AttachCopy(&result.ProductUri, product_uri_.raw_string());
  ::OpcUa_String_AttachCopy(
      &result.ApplicationName.Text,
      OpcUa_String_GetRawString(&application_name_.text()));
  ::OpcUa_String_AttachCopy(
      &result.ApplicationName.Locale,
      OpcUa_String_GetRawString(&application_name_.locale()));
  result.ApplicationType = OpcUa_ApplicationType_Server;

  Vector<OpcUa_String> discovery_urls(1);
  ::OpcUa_String_AttachCopy(&discovery_urls[0], url_.raw_string());
  result.NoOfDiscoveryUrls = discovery_urls.size();
  result.DiscoveryUrls = discovery_urls.release();

  return result;
}

inline EndpointDescription EndpointImpl::GetEndpointDescription() const {
  EndpointDescription result;

  ::OpcUa_String_AttachCopy(&result.EndpointUrl, url_.raw_string());
  GetApplicationDescription().release(result.Server);

  /*::OpcUa_String_AttachCopy(&result.SecurityPolicyUri,
  OpcUa_SecurityPolicy_Basic128Rsa15); result.SecurityMode =
  OpcUa_MessageSecurityMode_SignAndEncrypt;*/

  ::OpcUa_String_AttachCopy(&result.SecurityPolicyUri,
                            OpcUa_SecurityPolicy_None);
  result.SecurityMode = OpcUa_MessageSecurityMode_None;

  Vector<OpcUa_UserTokenPolicy> user_identity_tokens(2);
  {
    user_identity_tokens[0].TokenType = OpcUa_UserTokenType_UserName;
    ::OpcUa_String_AttachCopy(&user_identity_tokens[0].SecurityPolicyUri,
                              OpcUa_SecurityPolicy_None);
    ::OpcUa_String_AttachCopy(&user_identity_tokens[0].PolicyId, "3");
  }
  {
    user_identity_tokens[1].TokenType = OpcUa_UserTokenType_Anonymous;
    ::OpcUa_String_AttachCopy(&user_identity_tokens[1].PolicyId, "0");
  }
  result.NoOfUserIdentityTokens = user_identity_tokens.size();
  result.UserIdentityTokens = user_identity_tokens.release();

  ::OpcUa_String_AttachCopy(&result.TransportProfileUri,
                            OpcUa_TransportProfile_UaTcp);
  result.SecurityLevel = static_cast<OpcUa_Byte>(0);

  return result;
}

inline std::shared_ptr<Session> EndpointImpl::CreateSession(
    String session_name) {
  std::lock_guard<std::mutex> lock{mutex_};

  auto session_id = MakeSessionId();
  const auto authentication_token = MakeAuthenticationToken();

  if (session_name.is_null())
    session_name = "Session";  // TODO: Append session id

  auto session = std::make_shared<Session>(SessionContext{
      std::move(session_id),
      std::move(session_name),
      authentication_token,
      session_handlers_,
  });

  sessions_.emplace(authentication_token, session);

  return session;
}

inline std::shared_ptr<Session> EndpointImpl::GetSession(
    const NodeId& authentication_token) {
  std::lock_guard<std::mutex> lock{mutex_};
  auto i = sessions_.find(authentication_token);
  return i != sessions_.end() ? i->second : nullptr;
}

// static
inline EndpointImpl::Registry& EndpointImpl::registry() {
  static Registry registry;
  return registry;
}

// static
inline void EndpointImpl::AddEndpoint(OpcUa_Handle endpoint_handle,
                                      std::shared_ptr<EndpointImpl> endpoint) {
  std::lock_guard<std::mutex> lock{registry().mutex};
  registry().endpoints.emplace(endpoint_handle, std::move(endpoint));
}

// static
inline void EndpointImpl::RemoveEndpoint(OpcUa_Handle endpoint_handle) {
  std::lock_guard<std::mutex> lock{registry().mutex};
  registry().endpoints.erase(endpoint_handle);
}

// static
inline std::shared_ptr<EndpointImpl> EndpointImpl::GetEndpoint(
    OpcUa_Handle endpoint_handle) {
  std::lock_guard<std::mutex> lock{registry().mutex};
  auto i = registry().endpoints.find(endpoint_handle);
  return i != registry().endpoints.end() ? i->second : nullptr;
}

}  // namespace detail
}  // namespace server
}  // namespace opcua
