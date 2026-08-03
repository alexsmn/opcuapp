#pragma once

#include "opcua/types/localized_text.h"
#include "opcua/types/node_id.h"
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
  // Trust store used to authenticate the SERVER's application instance
  // certificate, mirroring the server-side store that authenticates clients.
  // All empty means the server is not authenticated at all: the certificate
  // arrives inside the discovered EndpointDescription, so without a store a
  // man in the middle can substitute its own and the SecureChannel is
  // encrypted to the attacker (OPC UA Part 4 §5.4.4 / Part 2 §4).
  std::string trusted_certificates_dir;
  std::string issuer_certificates_dir;
  std::string crl_dir;
  std::string rejected_certificates_dir;

  // True once any trust-store directory is configured. When set, endpoint
  // selection additionally refuses to fall back to an unsecured endpoint —
  // an operator who provisioned a trust store did not ask for None, and
  // letting `Auto` take None would hand a MITM a free downgrade.
  [[nodiscard]] bool has_trust_store() const {
    return !trusted_certificates_dir.empty() ||
           !issuer_certificates_dir.empty();
  }
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
