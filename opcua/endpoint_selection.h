#pragma once

#include "opcua/message.h"
#include "opcua/scada/status_or.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opcua {

// Standard OPC UA SecurityPolicy URIs the client knows about (OPC UA Part 7).
inline constexpr std::string_view kSecurityPolicyUriNone =
    "http://opcfoundation.org/UA/SecurityPolicy#None";
inline constexpr std::string_view kSecurityPolicyUriBasic256Sha256 =
    "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256";

// One (SecurityPolicy, MessageSecurityMode) pair the client can actually
// negotiate. The set of these is the client's capability list.
struct SupportedSecurity {
  std::string security_policy_uri;
  MessageSecurityMode security_mode = MessageSecurityMode::None;

  friend bool operator==(const SupportedSecurity&,
                         const SupportedSecurity&) = default;
};

// What this client build can speak. Derived from the modes ClientSecureChannel
// actually implements, so endpoint selection never picks a policy the channel
// can't open.
struct ClientCapabilities {
  std::vector<SupportedSecurity> supported;

  // The capabilities of the in-repo binary client: SecurityPolicy=None and
  // Basic256Sha256/SignAndEncrypt (Sign-only is not implemented).
  static ClientCapabilities Default();
};

// What the operator asked for when choosing an endpoint.
struct SecurityPreference {
  enum class Mode {
    // Pick the most secure supported endpoint the server offers.
    Auto,
    // Require an unsecured (SecurityPolicy=None) endpoint.
    None,
    // Require an encrypted (MessageSecurityMode=SignAndEncrypt) endpoint.
    SignAndEncrypt,
  };
  Mode mode = Mode::Auto;
  // Optional explicit SecurityPolicy URI to require, narrowing the choice
  // further than `mode` alone.
  std::optional<std::string> required_policy_uri;
};

// Chooses the endpoint that best satisfies `preference` among those whose
// (policy, mode) is in `capabilities`. With Mode::Auto the most secure
// supported endpoint wins, tie-broken by the server's advertised
// securityLevel. Returns a Bad status (StatusCode::Bad) when nothing the
// server offers is both supported and compatible with the preference; callers
// should log the offered-vs-supported sets for the operator.
[[nodiscard]] scada::StatusOr<EndpointDescription> SelectEndpoint(
    std::span<const EndpointDescription> endpoints,
    const SecurityPreference& preference,
    const ClientCapabilities& capabilities);

}  // namespace opcua
