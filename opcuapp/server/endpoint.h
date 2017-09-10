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

  OpcUa_Handle handle() const;
  const String& url() const;

  using Event = OpcUa_Endpoint_Event;
  using StatusHandler = std::function<void(Event event)>;

  void set_status_handler(StatusHandler handler);
  void set_read_handler(ReadHandler handler);
  void set_browse_handler(BrowseHandler handler);
  void set_create_monitored_item_handler(CreateMonitoredItemHandler handler);

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
            Span<const SecurityPolicyConfiguration> security_policies);

 private:
  class Core;

  std::shared_ptr<Core> core_;
};

} // namespace server
} // namespace opcua
