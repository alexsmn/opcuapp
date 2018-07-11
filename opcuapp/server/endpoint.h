#pragma once

#include <opcua_endpoint.h>
#include <opcuapp/server/handlers.h>

namespace opcua {
namespace server {

namespace detail {
class EndpointImpl;
}

class Endpoint {
 public:
  using SerializerType = OpcUa_Endpoint_SerializerType;

  explicit Endpoint(SerializerType serializer_type);
  ~Endpoint();

  OpcUa_Handle handle() const;
  const String& url() const;

  using Event = OpcUa_Endpoint_Event;
  using StatusHandler = std::function<void(Event event)>;

  void set_status_handler(StatusHandler handler);
  void set_read_handler(ReadHandler handler);
  void set_browse_handler(BrowseHandler handler);
  void set_create_monitored_item_handler(CreateMonitoredItemHandler handler);

  struct SecurityPolicyConfiguration
      : OpcUa_Endpoint_SecurityPolicyConfiguration {
    SecurityPolicyConfiguration() {
      ::OpcUa_String_Initialize(&sSecurityPolicy);
      ::OpcUa_String_AttachReadOnly(&sSecurityPolicy,
                                    OpcUa_SecurityPolicy_None);
      pbsClientCertificate = OpcUa_Null;
      uMessageSecurityModes = OPCUA_ENDPOINT_MESSAGESECURITYMODE_NONE;
    }

    ~SecurityPolicyConfiguration() {
      ::OpcUa_String_Clear(&sSecurityPolicy);
      ::OpcUa_ByteString_Clear(pbsClientCertificate);
    }
  };

  // WARNING: Referenced parameters must outlive the Endpoint.
  void Open(String url,
            bool listen_on_all_interfaces,
            const OpcUa_ByteString& server_certificate,
            const OpcUa_Key& server_private_key,
            const OpcUa_Void* pki_config,
            Span<const SecurityPolicyConfiguration> security_policies);

 private:
  const std::shared_ptr<detail::EndpointImpl> impl_;
};

}  // namespace server
}  // namespace opcua

#include <opcuapp/server/endpoint_impl.h>

namespace opcua {
namespace server {

// Endpoint

inline Endpoint::Endpoint(OpcUa_Endpoint_SerializerType serializer_type)
    : impl_{std::make_shared<detail::EndpointImpl>(serializer_type)} {}

inline Endpoint::~Endpoint() {
  impl_->Close();
}

inline void Endpoint::Open(
    String url,
    bool listen_on_all_interfaces,
    const OpcUa_ByteString& server_certificate,
    const OpcUa_Key& server_private_key,
    const OpcUa_Void* pki_config,
    Span<const SecurityPolicyConfiguration> security_policies) {
  impl_->Open(std::move(url), listen_on_all_interfaces, server_certificate,
              server_private_key, pki_config, security_policies);
}

inline OpcUa_Handle Endpoint::handle() const {
  return impl_->handle();
}

inline const String& Endpoint::url() const {
  return impl_->url();
}

inline void Endpoint::set_status_handler(StatusHandler handler) {
  impl_->set_status_handler(std::move(handler));
}

inline void Endpoint::set_read_handler(ReadHandler handler) {
  impl_->set_read_handler(std::move(handler));
}

inline void Endpoint::set_browse_handler(BrowseHandler handler) {
  impl_->set_browse_handler(std::move(handler));
}

inline void Endpoint::set_create_monitored_item_handler(
    CreateMonitoredItemHandler handler) {
  impl_->set_create_monitored_item_handler(std::move(handler));
}

}  // namespace server
}  // namespace opcua
