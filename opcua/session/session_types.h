#pragma once

#include "opcua/types/localized_text.h"
#include "opcua/types/node_id.h"
#include "opcua/types/privileges.h"
#include "opcua/types/status.h"

#include <boost/signals2/connection.hpp>
#include <functional>
#include <string>
#include <utility>

namespace opcua {

// Endpoint security selection for a session. Transport-neutral so the generic
// session connection parameters can map to concrete SecurityPolicy /
// MessageSecurityMode choices. The defaults (mode=None, empty paths) preserve
// the legacy direct-connect behaviour with no discovery.
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

}  // namespace opcua
