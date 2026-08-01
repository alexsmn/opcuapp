#pragma once

#include "opcua/client/endpoint_selection.h"
#include "opcua/session/session_types.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/types/status_or.h"
#include "opcua/ua/ua_types.h"

namespace opcua {

// Shared client-side security helpers.
//
// These live outside ClientSession because DiscoveryClient needs exactly the
// same three steps before it can talk over a secured channel — map the settings
// onto an endpoint preference, authenticate the server's certificate, build the
// channel security — and a second copy of that sequence is a second place for
// the trust check to be forgotten.

// Maps the transport-neutral SessionSecuritySettings onto the endpoint
// SecurityPreference used by SelectEndpoint.
//
// A configured trust store raises Auto's floor to SignAndEncrypt: "most secure
// on offer" is chosen from a list that unsecured discovery returned, so a peer
// advertising None only would otherwise be a silent downgrade. An operator who
// provisioned a trust store did not ask to keep talking unsecured; `mode:
// "none"` is how they would say that.
[[nodiscard]] SecurityPreference ToSecurityPreference(
    const SessionSecuritySettings& settings);

// The client capabilities implied by `settings`: without a client
// certificate/key only SecurityPolicy=None can be opened, so Auto must not
// choose a secured endpoint it cannot then establish.
[[nodiscard]] ClientCapabilities CapabilitiesFor(
    const SessionSecuritySettings& settings);

// Builds the secure-channel Security for a chosen endpoint: a None Security for
// SecurityPolicy=None, otherwise the client certificate/key from `settings`
// plus the server certificate carried by the endpoint.
//
// **This is where the server is authenticated.** The endpoint's
// serverCertificate arrived inside a GetEndpoints response fetched over an
// unsecured channel (OPC UA Part 4 §5.4.4 permits that — a client that does not
// yet know the server's certificate cannot encrypt to it), so it is
// attacker-controlled until the configured trust store validates it. Without a
// store the legacy trust-on-first-use behaviour is kept, so deployments that
// configure nothing are unaffected.
[[nodiscard]] StatusOr<binary::ClientSecureChannel::Security>
BuildChannelSecurity(const EndpointDescription& endpoint,
                     const SessionSecuritySettings& settings);

// True when the two endpoint descriptions agree on every security-relevant
// field: endpointUrl is deliberately NOT compared, because a server legitimately
// rewrites it per caller (ServerRuntime::ReachableEndpointUrl echoes the URL the
// client dialled), and a byte-equality check would then fail on every
// connection.
//
// Used for the Part 4 §5.4.4 anti-downgrade re-check: the endpoint list is
// re-fetched over the established SecureChannel and compared against what
// unsecured discovery returned. A mismatch means discovery was tampered with.
[[nodiscard]] bool SecurityEquivalent(const EndpointDescription& a,
                                      const EndpointDescription& b);

}  // namespace opcua
