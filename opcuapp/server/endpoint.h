#pragma once

#include <opcuapp/node_id.h>
#include <opcuapp/server/handlers.h>
#include <opcuapp/status_code.h>
#include <opcuapp/structs.h>
#include <opcuapp/types.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <opcua_endpoint.h>
#include <vector>

namespace opcua {
namespace server {

class Session;

class Endpoint {
 public:
  explicit Endpoint(OpcUa_Endpoint_SerializerType serializer_type);
  ~Endpoint();

  void set_read_handler(ReadHandler handler) { read_handler_ = std::move(handler); }
  void set_browse_handler(BrowseHandler handler) { browse_handler_ = std::move(handler); }
  void set_create_monitored_item_handler(CreateMonitoredItemHandler handler) { create_monitored_item_handler_ = std::move(handler); }

  using OpenCallback = std::function<void()>;

  struct SecurityPolicyConfiguration : OpcUa_Endpoint_SecurityPolicyConfiguration {
    SecurityPolicyConfiguration() {
      ::OpcUa_String_Initialize(&sSecurityPolicy);
      ::OpcUa_String_AttachReadOnly(&sSecurityPolicy, OpcUa_SecurityPolicy_None);
      pbsClientCertificate = OpcUa_Null;
      uMessageSecurityModes = OPCUA_ENDPOINT_MESSAGESECURITYMODE_NONE;
    }

    ~SecurityPolicyConfiguration() {
      ::OpcUa_String_Clear(&sSecurityPolicy);
      ::OpcUa_ByteString_Clear(pbsClientCertificate);
    }
  };

  // WARNING: Referenced parameters must outlive the Endpoint.
  void Open(String                                  url,
            bool                                    listen_on_all_interfaces,
            const OpcUa_ByteString&                 server_certificate,
            const OpcUa_Key&                        server_private_key,
            const OpcUa_Void*                       pki_config,
            Span<const SecurityPolicyConfiguration> security_policies,
            OpenCallback                            callback);

  OpcUa_Handle handle() const { return handle_; }

  const String& url() const { return url_; }

 private:
  class CallbackWrapper;

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

  String url_;
  ReadHandler read_handler_;
  BrowseHandler browse_handler_;
  CreateMonitoredItemHandler create_monitored_item_handler_;

  OpcUa_Endpoint handle_ = OpcUa_Null;

  std::mutex mutex_;
  std::map<NodeId /*authentication_token*/, std::shared_ptr<Session>> sessions_;
  unsigned next_session_id_ = 1;

  static std::map<OpcUa_Handle /*endpoint*/, Endpoint*> g_endpoints;
};

} // namespace server
} // namespace opcua
