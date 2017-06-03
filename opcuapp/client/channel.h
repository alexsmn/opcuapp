#pragma once

#include <opcua.h>
#include <opcua_channel.h>

#include "opcuapp/signal.h"
#include "opcuapp/status_code.h"

namespace opcua {
namespace client {

class Session;

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

  void set_url(std::string url) { url_ = std::move(url); }

  StatusCode status_code() const { return status_code_; }
  OpcUa_Channel handle() const { return handle_; }

  void Connect();
  void Disconnect();

  Signal<void(StatusCode status_code)> status_changed;

 private:
  void SetStatus(StatusCode status_code);

  static OpcUa_StatusCode OnConnectionStateChanged(OpcUa_Channel hChannel, OpcUa_Void* pCallbackData,
      OpcUa_Channel_Event eEvent, OpcUa_StatusCode uStatus);

  std::string url_;
  OpcUa_Channel handle_ = OpcUa_Null;

  mutable std::mutex mutex_;
  StatusCode status_code_{OpcUa_Bad};
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

inline void Channel::Connect() {
  opcua::ByteString client_private_key;
  OpcUa_P_OpenSSL_CertificateStore_Config pki_config{OpcUa_NO_PKI};
  opcua::String requested_security_policy_uri{OpcUa_SecurityPolicy_None};

  StatusCode status_code = OpcUa_Channel_BeginConnect(handle_,
      const_cast<OpcUa_StringA>(url_.c_str()),
      nullptr,
      client_private_key.pass(),
      nullptr,
      &pki_config,
      requested_security_policy_uri.pass(),
      0,
      OpcUa_MessageSecurityMode_None,
      10000,
      &Channel::OnConnectionStateChanged,
      this);

  if (!status_code)
    SetStatus(status_code);
}

inline void Channel::Disconnect() {
  // TODO:
}

void Channel::SetStatus(StatusCode status_code) {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    status_code_ = status_code;
  }
  status_changed(status_code);
}

// static
inline OpcUa_StatusCode Channel::OnConnectionStateChanged(OpcUa_Channel hChannel, OpcUa_Void* pCallbackData,
    OpcUa_Channel_Event eEvent, OpcUa_StatusCode uStatus) {
  assert(eEvent != eOpcUa_Channel_Event_Disconnected || OpcUa_IsBad(uStatus));
  static_cast<Channel*>(pCallbackData)->SetStatus(uStatus);
  return OpcUa_Good;
}

} // namespace client
} // namespace opcua
