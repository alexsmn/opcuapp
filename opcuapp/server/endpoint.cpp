#include "endpoint.h"

#include "opcuapp/requests.h"
#include "opcuapp/server/session.h"

#include <opcua_core.h>
#include <opcua_servicetable.h>

namespace opcua {
namespace server {

namespace {

template<class Response>
void SendResponse(OpcUa_Endpoint endpoint, OpcUa_Handle& context, const OpcUa_RequestHeader& request_header, Response& response) {
  PrepareResponse(request_header, response.ResponseHeader.ServiceResult, response.ResponseHeader);
  Response* raw_response = OpcUa_Null;
  OpcUa_EncodeableType* response_type = OpcUa_Null;
  Check(::OpcUa_Endpoint_BeginSendResponse(endpoint, context, (OpcUa_Void**)&raw_response, &response_type));
  *raw_response = response;
  Initialize(response);
  PrepareResponse(request_header, response.ResponseHeader.ServiceResult, raw_response->ResponseHeader);
  Check(::OpcUa_Endpoint_EndSendResponse(endpoint, &context, OpcUa_Good, raw_response, response_type));
  ::OpcUa_EncodeableObject_Delete(response_type, (OpcUa_Void**)&raw_response);
}

void SendFault(OpcUa_Endpoint endpoint, OpcUa_Handle& context, const OpcUa_RequestHeader& request_header, OpcUa_ResponseHeader& response_header) {
  OpcUa_Void* pFault = OpcUa_Null;
  OpcUa_EncodeableType* pFaultType = OpcUa_Null;
  Check(::OpcUa_ServerApi_CreateFault(
      const_cast<OpcUa_RequestHeader*>(&request_header),
      response_header.ServiceResult,
      &response_header.ServiceDiagnostics,
      &response_header.NoOfStringTable,
      &response_header.StringTable,
      &pFault,
      &pFaultType));
  Check(::OpcUa_Endpoint_EndSendResponse(endpoint, &context, OpcUa_Good, pFault, pFaultType));
  ::OpcUa_EncodeableObject_Delete(pFaultType, (OpcUa_Void**)&pFault);
}

} // namespace

class Endpoint::Core : public std::enable_shared_from_this<Core> {
 public:
  explicit Core(OpcUa_Endpoint_SerializerType serializer_type);

  OpcUa_Handle handle() const { return handle_; }
  const String& url() const { return url_; }

  void set_read_handler(ReadHandler handler) { read_handler_ = std::move(handler); }
  void set_browse_handler(BrowseHandler handler) { browse_handler_ = std::move(handler); }
  void set_create_monitored_item_handler(CreateMonitoredItemHandler handler) { create_monitored_item_handler_ = std::move(handler); }

  // WARNING: Referenced parameters must outlive the Endpoint.
  void Open(String                                  url,
            bool                                    listen_on_all_interfaces,
            const OpcUa_ByteString&                 server_certificate,
            const OpcUa_Key&                        server_private_key,
            const OpcUa_Void*                       pki_config,
            Span<const SecurityPolicyConfiguration> security_policies,
            OpenCallback                            callback);

  void Close();

 private:
  ApplicationDescription GetApplicationDescription() const;
  EndpointDescription GetEndpointDescription() const;

  NodeId MakeAuthenticationToken();
  NodeId MakeSessionId();

  std::shared_ptr<Session> CreateSession(String session_name);
  std::shared_ptr<Session> GetSession(const NodeId& authentication_token);

  std::vector<const OpcUa_ServiceType*> MakeSupportedServices() const;

  void BeginInvoke(OpcUa_GetEndpointsRequest& request, const std::function<void(OpcUa_GetEndpointsResponse& response)>& callback);
  void BeginInvoke(OpcUa_FindServersRequest& request, const std::function<void(OpcUa_FindServersResponse& response)>& callback);
  void BeginInvoke(OpcUa_CreateSessionRequest& request, const std::function<void(OpcUa_CreateSessionResponse& response)>& callback);

  template<class Request, class Response>
  static OpcUa_StatusCode BeginInvokeEndpoint(
      OpcUa_Endpoint        a_hEndpoint,
      OpcUa_Handle          a_hContext,
      OpcUa_Void**          a_ppRequest,
      OpcUa_EncodeableType* a_pRequestType);

  template<class Request, class Response>
  static OpcUa_StatusCode BeginInvokeSession(
      OpcUa_Endpoint        a_hEndpoint,
      OpcUa_Handle          a_hContext,
      OpcUa_Void**          a_ppRequest,
      OpcUa_EncodeableType* a_pRequestType);

  template<class Request, class Response>
  static OpcUa_StatusCode BeginInvokeSubscription(
      OpcUa_Endpoint        a_hEndpoint,
      OpcUa_Handle          a_hContext,
      OpcUa_Void**          a_ppRequest,
      OpcUa_EncodeableType* a_pRequestType);

  static opcua::Vector<OpcUa_EndpointDescription> GetEndpoints();
  static opcua::Vector<OpcUa_ApplicationDescription> FindServers();

  static OpcUa_StatusCode Invoke(
      OpcUa_Endpoint          hEndpoint,
      OpcUa_Void*             pvCallbackData,
      OpcUa_Endpoint_Event    eEvent,
      OpcUa_StatusCode        uStatus,
      OpcUa_UInt32            uSecureChannelId,
      OpcUa_ByteString*       pbsClientCertificate,
      OpcUa_String*           pSecurityPolicy,
      OpcUa_UInt16            uSecurityMode);

  String url_;
  OpenCallback open_callback_;
  ReadHandler read_handler_;
  BrowseHandler browse_handler_;
  CreateMonitoredItemHandler create_monitored_item_handler_;

  OpcUa_Endpoint handle_ = OpcUa_Null;

  std::mutex mutex_;
  std::map<NodeId /*authentication_token*/, std::shared_ptr<Session>> sessions_;
  unsigned next_session_id_ = 1;

  static std::map<OpcUa_Handle /*endpoint*/, std::shared_ptr<Core>> g_endpoints;
};

// static
inline OpcUa_StatusCode Endpoint::Core::Invoke(
      OpcUa_Endpoint          hEndpoint,
      OpcUa_Void*             pvCallbackData,
      OpcUa_Endpoint_Event    eEvent,
      OpcUa_StatusCode        uStatus,
      OpcUa_UInt32            uSecureChannelId,
      OpcUa_ByteString*       pbsClientCertificate,
      OpcUa_String*           pSecurityPolicy,
      OpcUa_UInt16            uSecurityMode) {
  auto& core = *static_cast<Core*>(pvCallbackData);
  core.open_callback_();
  // TODO: Delete.
  return OpcUa_Good;
}

// Endpoint

std::map<OpcUa_Handle /*endpoint*/, std::shared_ptr<Endpoint::Core>> Endpoint::Core::g_endpoints;

Endpoint::Endpoint(OpcUa_Endpoint_SerializerType serializer_type)
    : core_{std::make_shared<Core>(serializer_type)} {
}

Endpoint::Core::Core(OpcUa_Endpoint_SerializerType serializer_type) {
  Check(::OpcUa_Endpoint_Create(&handle_, serializer_type, const_cast<OpcUa_ServiceType**>(MakeSupportedServices().data())));
}

Endpoint::~Endpoint() {
  core_->Close();
}

void Endpoint::Core::Close() {
  if (handle_ != OpcUa_Null) {
    g_endpoints.erase(handle_);
    ::OpcUa_Endpoint_Delete(&handle_);
  }
}

void Endpoint::Open(String                                  url,
                    bool                                    listen_on_all_interfaces,
                    const OpcUa_ByteString&                 server_certificate,
                    const OpcUa_Key&                        server_private_key,
                    const OpcUa_Void*                       pki_config,
                    Span<const SecurityPolicyConfiguration> security_policies,
                    OpenCallback                            callback) {
  core_->Open(std::move(url),
              listen_on_all_interfaces,
              server_certificate,
              server_private_key,
              pki_config,
              security_policies,
              std::move(callback));
}

void Endpoint::Core::Open(String                                  url,
                    bool                                    listen_on_all_interfaces,
                    const OpcUa_ByteString&                 server_certificate,
                    const OpcUa_Key&                        server_private_key,
                    const OpcUa_Void*                       pki_config,
                    Span<const SecurityPolicyConfiguration> security_policies,
                    OpenCallback                            callback) {
  g_endpoints.emplace(handle_, shared_from_this());

  url_ = std::move(url);
  open_callback_ = std::move(callback);

  Check(::OpcUa_Endpoint_Open(
      handle_, 
      url_.raw_string(),
      listen_on_all_interfaces ? OpcUa_True : OpcUa_False,
      &Core::Invoke,
      this,
      &const_cast<OpcUa_ByteString&>(server_certificate),
      &const_cast<OpcUa_Key&>(server_private_key),
      const_cast<OpcUa_Void*>(pki_config),
      security_policies.size(),
      const_cast<SecurityPolicyConfiguration*>(security_policies.data())));
}

std::vector<const OpcUa_ServiceType*> Endpoint::Core::MakeSupportedServices() const {
  static const OpcUa_ServiceType kServiceTypes[] = {
      {
        OpcUaId_GetEndpointsRequest,
        &OpcUa_GetEndpointsResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeEndpoint<OpcUa_GetEndpointsRequest, OpcUa_GetEndpointsResponse>),
      },
      {
        OpcUaId_FindServersRequest,
        &OpcUa_FindServersResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeEndpoint<OpcUa_FindServersRequest, OpcUa_FindServersResponse>),
      },
      {
        OpcUaId_CreateSessionRequest,
        &OpcUa_CreateSessionResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeEndpoint<OpcUa_CreateSessionRequest, OpcUa_CreateSessionResponse>),
      },
      {
        OpcUaId_ActivateSessionRequest,
        &OpcUa_ActivateSessionResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSession<OpcUa_ActivateSessionRequest, OpcUa_ActivateSessionResponse>),
      },
      {
        OpcUaId_CloseSessionRequest,
        &OpcUa_CloseSessionResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSession<OpcUa_CloseSessionRequest, OpcUa_CloseSessionResponse>),
      },
      {
        OpcUaId_ReadRequest,
        &OpcUa_ReadResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSession<OpcUa_ReadRequest, OpcUa_ReadResponse>),
      },
      {
        OpcUaId_BrowseRequest,
        &OpcUa_BrowseResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSession<OpcUa_BrowseRequest, OpcUa_BrowseResponse>),
      },
      {
        OpcUaId_CreateSubscriptionRequest,
        &OpcUa_CreateSubscriptionResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSession<OpcUa_CreateSubscriptionRequest, OpcUa_CreateSubscriptionResponse>),
      },
      {
        OpcUaId_CreateMonitoredItemsRequest,
        &OpcUa_CreateMonitoredItemsResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSubscription<OpcUa_CreateMonitoredItemsRequest, OpcUa_CreateMonitoredItemsResponse>),
      },
      {
        OpcUaId_DeleteMonitoredItemsRequest,
        &OpcUa_DeleteMonitoredItemsResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSubscription<OpcUa_DeleteMonitoredItemsRequest, OpcUa_DeleteMonitoredItemsResponse>),
      },
      {
        OpcUaId_PublishRequest,
        &OpcUa_PublishResponse_EncodeableType,
        static_cast<OpcUa_PfnBeginInvokeService*>(&BeginInvokeSession<OpcUa_PublishRequest, OpcUa_PublishResponse>),
      },
  };

  std::vector<const OpcUa_ServiceType*> result(std::size(kServiceTypes) + 1, OpcUa_Null);
  std::transform(std::begin(kServiceTypes), std::end(kServiceTypes), result.begin(), [](auto& v) { return &v; });
  return result;
}

template<class Request, class Response>
OpcUa_StatusCode Endpoint::Core::BeginInvokeEndpoint(
    OpcUa_Endpoint        a_hEndpoint,
    OpcUa_Handle          a_hContext,
    OpcUa_Void**          a_ppRequest,
    OpcUa_EncodeableType* a_pRequestType) {
  auto& request = *reinterpret_cast<Request*>(*a_ppRequest);

  auto i = g_endpoints.find(a_hEndpoint);
  if (i == g_endpoints.end()) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    SendFault(a_hEndpoint, a_hContext, request.RequestHeader, response_header);
    return OpcUa_Good;
  }

  auto& server = *i->second;
  server.BeginInvoke(request, [a_hEndpoint, a_hContext, request_header = request.RequestHeader](Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      SendResponse(a_hEndpoint, a_hContext, request_header, response);
    else
      SendFault(a_hEndpoint, a_hContext, request_header, response.ResponseHeader);
  });

  return OpcUa_Good;
}

template<class Request, class Response>
OpcUa_StatusCode Endpoint::Core::BeginInvokeSession(
    OpcUa_Endpoint        a_hEndpoint,
    OpcUa_Handle          a_hContext,
    OpcUa_Void**          a_ppRequest,
    OpcUa_EncodeableType* a_pRequestType) {
  auto& request = *reinterpret_cast<Request*>(*a_ppRequest);

  auto i = g_endpoints.find(a_hEndpoint);
  if (i == g_endpoints.end()) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    SendFault(a_hEndpoint, a_hContext, request.RequestHeader, response_header);
    return OpcUa_Good;
  }

  auto& server = *i->second;

  auto session = server.GetSession(request.RequestHeader.AuthenticationToken);
  if (!session) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    SendFault(a_hEndpoint, a_hContext, request.RequestHeader, response_header);
    return OpcUa_Good;
  }

  // TODO: Remove.
  session->GetSubscription(1);

  session->BeginInvoke(request, [a_hEndpoint, a_hContext, request_header = request.RequestHeader](Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      SendResponse(a_hEndpoint, a_hContext, request_header, response);
    else
      SendFault(a_hEndpoint, a_hContext, request_header, response.ResponseHeader);
  });

  return OpcUa_Good;
}

template<class Request, class Response>
OpcUa_StatusCode Endpoint::Core::BeginInvokeSubscription(
    OpcUa_Endpoint        a_hEndpoint,
    OpcUa_Handle          a_hContext,
    OpcUa_Void**          a_ppRequest,
    OpcUa_EncodeableType* a_pRequestType) {
  auto& request = *reinterpret_cast<Request*>(*a_ppRequest);

  auto i = g_endpoints.find(a_hEndpoint);
  if (i == g_endpoints.end()) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    SendFault(a_hEndpoint, a_hContext, request.RequestHeader, response_header);
    return OpcUa_Good;
  }

  auto& endpoint = *i->second;

  auto session = endpoint.GetSession(request.RequestHeader.AuthenticationToken);
  auto subscription = session ? session->GetSubscription(request.SubscriptionId) : nullptr;
  if (!subscription) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    SendFault(a_hEndpoint, a_hContext, request.RequestHeader, response_header);
    return OpcUa_Good;
  }

  subscription->BeginInvoke(request, [a_hEndpoint, a_hContext, request_header = request.RequestHeader](Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      SendResponse(a_hEndpoint, a_hContext, request_header, response);
    else
      SendFault(a_hEndpoint, a_hContext, request_header, response.ResponseHeader);
  });

  return OpcUa_Good;
}

void PrepareResponse(const OpcUa_RequestHeader& request_header, OpcUa_StatusCode status_code, OpcUa_ResponseHeader& response_header) {
  ::OpcUa_ResponseHeader_Initialize(&response_header);
  response_header.RequestHandle = request_header.RequestHandle;

  /*auto duration = GetDateTimeDiff(request_header.Timestamp, ::OpcUa_DateTime_UtcNow());
  if (duration > std::chrono::seconds(1))
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
void Endpoint::Core::BeginInvoke(OpcUa_GetEndpointsRequest& request, const std::function<void(OpcUa_GetEndpointsResponse& response)>& callback) {
  Span<const OpcUa_String> profile_uris{request.ProfileUris, static_cast<size_t>(request.NoOfProfileUris)};

  GetEndpointsResponse response;

  if (!profile_uris.empty()) {
    PrepareResponse(request.RequestHeader, OpcUa_BadNotImplemented, response.ResponseHeader);
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
Vector<OpcUa_EndpointDescription> Endpoint::Core::GetEndpoints() {
  Vector<OpcUa_EndpointDescription> endpoints(g_endpoints.size());
  size_t i = 0;
  for (auto& p : g_endpoints)
    p.second->GetEndpointDescription().release(endpoints[i++]);
  return endpoints;
}

// static
void Endpoint::Core::BeginInvoke(OpcUa_FindServersRequest& request, const std::function<void(OpcUa_FindServersResponse& response)>& callback) {
  Span<const OpcUa_String> server_uris{request.ServerUris, static_cast<size_t>(request.NoOfServerUris)};
  Span<const OpcUa_String> locale_ids{request.LocaleIds, static_cast<size_t>(request.NoOfLocaleIds)};

  FindServersResponse response;

  if (!server_uris.empty()) {
    PrepareResponse(request.RequestHeader, OpcUa_BadNotImplemented, response.ResponseHeader);
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
Vector<OpcUa_ApplicationDescription> Endpoint::Core::FindServers() {
  Vector<OpcUa_ApplicationDescription> servers(g_endpoints.size());
  size_t index = 0;
  for (auto& p : g_endpoints)
    p.second->GetApplicationDescription().release(servers[index++]);
  return servers;
}

NodeId Endpoint::Core::MakeAuthenticationToken() {
  // TODO: Namespace index.
  return NodeId{"FixMe", 101};
}

NodeId Endpoint::Core::MakeSessionId() {
  // TODO: Namespace index.
  return NodeId{next_session_id_++, 100};
}

// static
void Endpoint::Core::BeginInvoke(OpcUa_CreateSessionRequest& request, const std::function<void(OpcUa_CreateSessionResponse& response)>& callback) {
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

ApplicationDescription Endpoint::Core::GetApplicationDescription() const {
  ApplicationDescription result;
  ::OpcUa_String_AttachCopy(&result.ApplicationUri, "Nano_Server");
  ::OpcUa_String_AttachCopy(&result.ProductUri, "ProductUri");
  ::OpcUa_String_AttachCopy(&result.ApplicationName.Text, "Nano_Server");
  ::OpcUa_String_AttachCopy(&result.ApplicationName.Locale, "en");
  result.ApplicationType = OpcUa_ApplicationType_Server;

  Vector<OpcUa_String> discovery_urls(1);
  ::OpcUa_String_AttachCopy(&discovery_urls[0], url_.raw_string());
  result.NoOfDiscoveryUrls = discovery_urls.size();
  result.DiscoveryUrls = discovery_urls.release();

  return result;
}

EndpointDescription Endpoint::Core::GetEndpointDescription() const {
  EndpointDescription result;

  ::OpcUa_String_AttachCopy(&result.EndpointUrl, url_.raw_string());
  GetApplicationDescription().release(result.Server);

  /*::OpcUa_String_AttachCopy(&result.SecurityPolicyUri, OpcUa_SecurityPolicy_Basic128Rsa15);
  result.SecurityMode = OpcUa_MessageSecurityMode_SignAndEncrypt;*/

  ::OpcUa_String_AttachCopy(&result.SecurityPolicyUri, OpcUa_SecurityPolicy_None);
  result.SecurityMode = OpcUa_MessageSecurityMode_None;

  Vector<OpcUa_UserTokenPolicy> user_identity_tokens(2);
  {
    user_identity_tokens[0].TokenType = OpcUa_UserTokenType_UserName;
    ::OpcUa_String_AttachCopy(&user_identity_tokens[0].SecurityPolicyUri, OpcUa_SecurityPolicy_None);
    ::OpcUa_String_AttachCopy(&user_identity_tokens[0].PolicyId, "3");
  }
  {
    user_identity_tokens[1].TokenType = OpcUa_UserTokenType_Anonymous;
    ::OpcUa_String_AttachCopy(&user_identity_tokens[1].PolicyId, "0");
  }
  result.NoOfUserIdentityTokens = user_identity_tokens.size();
  result.UserIdentityTokens = user_identity_tokens.release();

  ::OpcUa_String_AttachCopy(&result.TransportProfileUri, OpcUa_TransportProfile_UaTcp);
  result.SecurityLevel = static_cast<OpcUa_Byte>(0);

  return result;
}

std::shared_ptr<Session> Endpoint::Core::CreateSession(String session_name) {
  std::lock_guard<std::mutex> lock{mutex_};

  auto session_id = MakeSessionId();
  const auto authentication_token = MakeAuthenticationToken();

  if (session_name.is_null())
    session_name = "Session"; // TODO: Append session id

  auto session = std::make_shared<Session>(SessionContext{
      std::move(session_id),
      std::move(session_name),
      authentication_token,
      read_handler_,
      browse_handler_,
      create_monitored_item_handler_,
  });

  sessions_.emplace(authentication_token, session);

  return session;
}

std::shared_ptr<Session> Endpoint::Core::GetSession(const NodeId& authentication_token) {
  std::lock_guard<std::mutex> lock{mutex_};
  auto i = sessions_.find(authentication_token);
  return i != sessions_.end() ? i->second : nullptr;
}

OpcUa_Handle Endpoint::handle() const {
  return core_->handle();
}

const String& Endpoint::url() const {
  return core_->url();
}

void Endpoint::set_read_handler(ReadHandler handler) {
  core_->set_read_handler(std::move(handler));
}

void Endpoint::set_browse_handler(BrowseHandler handler) {
  core_->set_browse_handler(std::move(handler));
}

void Endpoint::set_create_monitored_item_handler(CreateMonitoredItemHandler handler) {
  core_->set_create_monitored_item_handler(std::move(handler));
}

} // namespace server
} // namespace opcua