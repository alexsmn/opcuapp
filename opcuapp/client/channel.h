#pragma once

#include <functional>
#include <opcua.h>
#include <opcua_channel.h>

#include "opcuapp/status_code.h"

namespace opcua {
namespace client {

struct ChannelContext {
  const OpcUa_StringA       url;
  const OpcUa_ByteString*   client_certificate;
  const OpcUa_Key*          client_private_key;
  const OpcUa_ByteString*   server_certificate;
  const OpcUa_Void*         pki_config;
  const OpcUa_String*       requested_security_policy_uri;
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
  // WARNING: |context| references must outlive the Channel.
  void Connect(const ChannelContext& context, const ConnectionStateHandler& connection_state_handler);
  void Disconnect();

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
  assert(context.client_certificate);
  assert(context.client_private_key);
  assert(context.server_certificate);

  connection_state_handler_ = connection_state_handler;

  StatusCode status_code = OpcUa_Channel_BeginConnect(handle_,
      context.url,
      const_cast<OpcUa_ByteString*>(context.client_certificate),
      const_cast<OpcUa_Key*>(context.client_private_key),
      const_cast<OpcUa_ByteString*>(context.server_certificate),
      const_cast<OpcUa_Void*>(context.pki_config),
      const_cast<OpcUa_String*>(context.requested_security_policy_uri),
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

inline void Channel::Disconnect() {
  StatusCode status_code = OpcUa_Channel_BeginDisconnect(handle_, &Channel::ConnectionStateChangedHelper, this);
  if (!status_code) {
    auto connection_state_handler = std::move(connection_state_handler_);
    connection_state_handler_ = nullptr;
    connection_state_handler(status_code, eOpcUa_Channel_Event_Disconnected);
  }
}

inline void Channel::Reset() {
  if (handle_)
    OpcUa_Channel_Delete(&handle_);
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
