#pragma once

#include <functional>
#include <opcua.h>
#include <opcua_channel.h>

#include "opcuapp/status_code.h"

namespace opcua {
namespace client {

struct ChannelContext {
  OpcUa_StringA             url;
  OpcUa_ByteString*         client_certificate;
  OpcUa_ByteString*         client_private_key;
  OpcUa_ByteString*         server_certificate;
  OpcUa_Void*               pki_config;
  OpcUa_String*             requested_security_policy_uri;
  OpcUa_Int32               requested_lifetime;
  OpcUa_MessageSecurityMode message_security_mode;
  OpcUa_UInt32              network_timeout_ms;
};

class Channel {
 public:
  explicit Channel(OpcUa_Channel_SerializerType serializer_type);
  Channel(OpcUa_Channel_SerializerType serializer_type, StatusCode& status_code);
  ~Channel();

  OpcUa_Channel handle() const { return handle_; }

  using ConnectionStateHandler = std::function<void(StatusCode status_code, OpcUa_Channel_Event event)>;
  void Connect(const ChannelContext& context, const ConnectionStateHandler& connection_state_handler);

  void Reset();

 private:
  static OpcUa_StatusCode ConnectionStateChangedHelper(OpcUa_Channel hChannel, OpcUa_Void* pCallbackData,
      OpcUa_Channel_Event eEvent, OpcUa_StatusCode uStatus);

  OpcUa_Channel handle_ = OpcUa_Null;
  ConnectionStateHandler connection_state_handler_;
};

inline Channel::Channel(OpcUa_Channel_SerializerType serializer_type) {
  Check(OpcUa_Channel_Create(&handle_, serializer_type));
}

inline Channel::Channel(OpcUa_Channel_SerializerType serializer_type, StatusCode& status_code) {
  status_code = OpcUa_Channel_Create(&handle_, serializer_type);
}

inline Channel::~Channel() {
  if (handle_)
    OpcUa_Channel_Delete(&handle_);
}

inline void Channel::Connect(const ChannelContext& context, const ConnectionStateHandler& connection_state_handler) {
  connection_state_handler_ = connection_state_handler;

  StatusCode status_code = OpcUa_Channel_BeginConnect(handle_,
      context.url,
      context.client_certificate,
      context.client_private_key,
      context.server_certificate,
      context.pki_config,
      context.requested_security_policy_uri,
      context.requested_lifetime,
      context.message_security_mode,
      context.network_timeout_ms,
      &Channel::ConnectionStateChangedHelper,
      this);

  if (!status_code) {
    connection_state_handler_ = nullptr;
    connection_state_handler(status_code, eOpcUa_Channel_Event_Disconnected);
  }
}

inline void Channel::Reset() {
  // TODO:
}

// static
inline OpcUa_StatusCode Channel::ConnectionStateChangedHelper(OpcUa_Channel hChannel, OpcUa_Void* pCallbackData,
    OpcUa_Channel_Event eEvent, OpcUa_StatusCode uStatus) {
  auto& channel = *static_cast<Channel*>(pCallbackData);
  auto handler = channel.connection_state_handler_;
  if (eEvent == eOpcUa_Channel_Event_Disconnected)
    channel.connection_state_handler_ = nullptr;
  handler(uStatus, eEvent);
  return OpcUa_Good;
}

} // namespace client
} // namespace opcua
