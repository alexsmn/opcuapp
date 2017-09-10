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

std::map<OpcUa_Handle /*endpoint*/, Endpoint*> Endpoint::g_endpoints;

Endpoint::Endpoint(OpcUa_Endpoint_SerializerType serializer_type) {
  Check(::OpcUa_Endpoint_Create(&handle_, serializer_type, const_cast<OpcUa_ServiceType**>(MakeSupportedServices().data())));
}

Endpoint::~Endpoint() {
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
                    Callback                                callback) {
  url_ = std::move(url);

  auto callback_wrapper = std::make_unique<EndpointCallbackWrapper>(std::move(callback));
  Check(::OpcUa_Endpoint_Open(
      handle_, 
      url_.raw_string(),
      listen_on_all_interfaces ? OpcUa_True : OpcUa_False,
      &EndpointCallbackWrapper::Invoke,
      callback_wrapper.release(),
      &const_cast<OpcUa_ByteString&>(server_certificate),
      &const_cast<OpcUa_Key&>(server_private_key),
      const_cast<OpcUa_Void*>(pki_config),
      security_policies.size(),
      const_cast<SecurityPolicyConfiguration*>(security_policies.data())));
  g_endpoints.emplace(handle_, this);
}

std::vector<const OpcUa_ServiceType*> Endpoint::MakeSupportedServices() const {
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
OpcUa_StatusCode Endpoint::BeginInvokeEndpoint(
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
OpcUa_StatusCode Endpoint::BeginInvokeSession(
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

  auto* session = server.GetSession(request.RequestHeader.AuthenticationToken);
  if (!session) {
    ResponseHeader response_header;
    // TODO: Valid error.
    response_header.ServiceResult = OpcUa_Bad;
    SendFault(a_hEndpoint, a_hContext, request.RequestHeader, response_header);
    return OpcUa_Good;
  }

  session->BeginInvoke(request, [a_hEndpoint, a_hContext, request_header = request.RequestHeader](Response& response) mutable {
    if (OpcUa_IsGood(response.ResponseHeader.ServiceResult))
      SendResponse(a_hEndpoint, a_hContext, request_header, response);
    else
      SendFault(a_hEndpoint, a_hContext, request_header, response.ResponseHeader);
  });

  return OpcUa_Good;
}

template<class Request, class Response>
OpcUa_StatusCode Endpoint::BeginInvokeSubscription(
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

  auto* session = endpoint.GetSession(request.RequestHeader.AuthenticationToken);
  auto* subscription = session ? session->GetSubscription(request.SubscriptionId) : nullptr;
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
void Endpoint::BeginInvoke(OpcUa_GetEndpointsRequest& request, const std::function<void(OpcUa_GetEndpointsResponse& response)>& callback) {
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
Vector<OpcUa_EndpointDescription> Endpoint::GetEndpoints() {
  Vector<OpcUa_EndpointDescription> endpoints(g_endpoints.size());
  size_t i = 0;
  for (auto& p : g_endpoints)
    p.second->GetEndpointDescription().release(endpoints[i++]);
  return endpoints;
}

// static
void Endpoint::BeginInvoke(OpcUa_FindServersRequest& request, const std::function<void(OpcUa_FindServersResponse& response)>& callback) {
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
Vector<OpcUa_ApplicationDescription> Endpoint::FindServers() {
  Vector<OpcUa_ApplicationDescription> servers(g_endpoints.size());
  size_t index = 0;
  for (auto& p : g_endpoints)
    p.second->GetApplicationDescription().release(servers[index++]);
  return servers;
}

NodeId Endpoint::MakeAuthenticationToken() {
  // TODO: Namespace index.
  return NodeId{"FixMe", 101};
}

NodeId Endpoint::MakeSessionId() {
  // TODO: Namespace index.
  return NodeId{next_session_id_++, 100};
}

// static
void Endpoint::BeginInvoke(OpcUa_CreateSessionRequest& request, const std::function<void(OpcUa_CreateSessionResponse& response)>& callback) {
  auto* session = CreateSession(std::move(request.SessionName));

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

ApplicationDescription Endpoint::GetApplicationDescription() const {
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

EndpointDescription Endpoint::GetEndpointDescription() const {
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

Session* Endpoint::CreateSession(String session_name) {
  auto session_id = MakeSessionId();
  const auto authentication_token = MakeAuthenticationToken();

  if (session_name.is_null())
    session_name = "Session"; // TODO: Append session id

  SessionContext context{
      std::move(session_id),
      std::move(session_name),
      authentication_token,
      read_handler_,
      browse_handler_,
      create_monitored_item_handler_,
  };
  auto i = sessions_.emplace(std::piecewise_construct,
      std::forward_as_tuple(authentication_token),
      std::forward_as_tuple(std::move(context)));
  return &i.first->second;
}

Session* Endpoint::GetSession(const NodeId& authentication_token) {
  auto i = sessions_.find(authentication_token);
  return i != sessions_.end() ? &i->second : nullptr;
}

} // namespace server
} // namespace opcua