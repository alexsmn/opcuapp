#pragma once

#include <functional>
#include <opcua.h>
#include <opcua_channel.h>

#include "opcuapp/status_code.h"

namespace opcua {
namespace client {

using StatusCallback = std::function<void(StatusCode status_code)>;

class Channel {
 public:
  explicit Channel(OpcUa_Channel_SerializerType serializer_type);
  Channel(OpcUa_Channel_SerializerType serializer_type, StatusCode& status_code);
  ~Channel();

  void set_url(std::string url) { url_ = std::move(url); }

  using ConnectionStateHandler = std::function<void(OpcUa_Channel_Event event)>;
  void set_connection_state_handler(ConnectionStateHandler handler) { connection_state_handler_ = std::move(handler); }

  void Connect(StatusCallback callback);
  void Disconnect(StatusCallback callback);

  OpcUa_Channel handle() const { return handle_; }

 private:
  void Reset();

  static OpcUa_StatusCode ConnectionStateChangedHelper(OpcUa_Channel hChannel, OpcUa_Void* pCallbackData,
      OpcUa_Channel_Event eEvent, OpcUa_StatusCode uStatus);

  std::string url_;
  ConnectionStateHandler connection_state_handler_;

  OpcUa_Channel handle_ = OpcUa_Null;
  StatusCallback connect_callback_;
  StatusCallback disconnect_callback_;
};

inline Channel::Channel(OpcUa_Channel_SerializerType serializer_type) {
  Check(OpcUa_Channel_Create(&handle_, serializer_type));
}

inline Channel::Channel(OpcUa_Channel_SerializerType serializer_type, StatusCode& status_code) {
  status_code = OpcUa_Channel_Create(&handle_, serializer_type);
}

inline Channel::~Channel() {
  OpcUa_Channel_Delete(&handle_);
}

inline void Channel::Connect(StatusCallback callback) {
  assert(!connect_callback_);
  connect_callback_ = std::move(callback);

  ByteString client_private_key;
  OpcUa_P_OpenSSL_CertificateStore_Config pki_config{OpcUa_NO_PKI};
  String requested_security_policy_uri{OpcUa_SecurityPolicy_None};

  StatusCode status_code = ::OpcUa_Channel_BeginConnect(handle_,
      const_cast<OpcUa_StringA>(url_.c_str()),
      nullptr,
      client_private_key.pass(),
      nullptr,
      &pki_config,
      requested_security_policy_uri.pass(),
      0,
      OpcUa_MessageSecurityMode_None,
      10000,
      &Channel::ConnectionStateChangedHelper,
      this);

  if (!status_code && connect_callback_)
    connect_callback_(status_code);
}

inline void Channel::Disconnect(StatusCallback callback) {
  assert(!disconnect_callback_);
  disconnect_callback_ = std::move(callback);

  StatusCode status_code = ::OpcUa_Channel_BeginDisconnect(handle_, &Channel::ConnectionStateChangedHelper, this);
  if (!status_code && disconnect_callback_)
    disconnect_callback_(status_code);
}

inline void Channel::Reset() {
  // TODO:
  connect_callback_ = nullptr;
  disconnect_callback_ = nullptr;
}

// static
inline OpcUa_StatusCode Channel::ConnectionStateChangedHelper(OpcUa_Channel hChannel, OpcUa_Void* pCallbackData,
    OpcUa_Channel_Event eEvent, OpcUa_StatusCode uStatus) {
  auto& channel = *static_cast<Channel*>(pCallbackData);
  if (channel.connection_state_handler_)
    channel.connection_state_handler_(eEvent);
  if (channel.connect_callback_) {
    auto connect_callback = std::move(channel.connect_callback_);
    channel.connect_callback_ = nullptr;
    connect_callback(uStatus);
  }
  if (channel.disconnect_callback_) {
    auto disconnect_callback = std::move(channel.connect_callback_);
    channel.disconnect_callback_ = nullptr;
    disconnect_callback(uStatus);
  }
  return OpcUa_Good;
}

} // namespace client
} // namespace opcua
