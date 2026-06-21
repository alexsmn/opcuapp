#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/node_id.h"
#include "opcua/scada/privileges.h"
#include "opcua/scada/status.h"

#include <boost/signals2/connection.hpp>
#include <functional>
#include <string>
#include <utility>

namespace opcua {
namespace base {
class TimeDelta;
}


class SessionDebugger;
class Status;

// Endpoint security selection for a session. Transport-neutral so the generic
// SessionService contract stays backend-agnostic; the OPC UA backend maps it to
// concrete SecurityPolicy / MessageSecurityMode choices. The defaults
// (mode=None, empty paths) preserve the legacy direct-connect behaviour with no
// discovery, so non-OPC-UA backends and existing callers are unaffected.
struct SessionSecuritySettings {
  enum class Mode {
    // No discovery; connect directly with no security (legacy behaviour).
    None,
    // Run discovery (GetEndpoints) and pick the most secure endpoint the
    // client supports.
    Auto,
    // Run discovery and require an encrypted (SignAndEncrypt) endpoint.
    SignAndEncrypt,
  };
  Mode mode = Mode::None;
  // Optional explicit SecurityPolicy URI to require, narrowing Auto selection.
  std::string required_policy_uri;
  // PEM file paths for the client application instance certificate and its
  // private key. Required when `mode` selects a secured endpoint.
  std::string client_certificate_path;
  std::string client_private_key_path;
};

struct SessionConnectParams {
  // The host name can be followed by a colon and a port number. If empty, then
  // the `connection_string` is used.
  std::string host;
  // The connection string defines a `transport::TransportString`. It's used if
  // the `host` is empty.
  std::string connection_string;
  LocalizedText user_name;
  LocalizedText password;
  bool allow_remote_logoff = false;
  // How to negotiate endpoint security. Defaults to the legacy unsecured path.
  SessionSecuritySettings security;
};

class SessionService {
 public:
  virtual ~SessionService() = default;

  virtual Awaitable<void> Connect(SessionConnectParams params) = 0;

  virtual Awaitable<Status> ConnectStatus(SessionConnectParams params) {
    co_await Connect(std::move(params));
    co_return StatusCode::Good;
  }

  virtual Awaitable<void> Reconnect() = 0;

  virtual Awaitable<void> Disconnect() = 0;

  virtual bool IsConnected(opcua::base::TimeDelta* ping_delay = nullptr) const = 0;

  virtual NodeId GetUserId() const = 0;
  virtual bool HasPrivilege(Privilege privilege) const = 0;

  virtual std::string GetHostName() const = 0;

  // TODO: Describe and explain the need.
  virtual bool IsScada() const = 0;

  using SessionStateChangedCallback =
      std::function<void(bool connected, const Status& status)>;

  virtual boost::signals2::scoped_connection SubscribeSessionStateChanged(
      const SessionStateChangedCallback& callback) = 0;

  virtual SessionDebugger* GetSessionDebugger() = 0;
};

}  // namespace opcua (vendored)
