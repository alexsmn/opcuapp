#include "opcua/client/endpoint_selection.h"

#include <algorithm>

namespace opcua {
namespace {

// Higher is more secure. Used to rank endpoints under Mode::Auto.
int SecurityRank(MessageSecurityMode mode) {
  switch (mode) {
    case MessageSecurityMode::SignAndEncrypt:
      return 3;
    case MessageSecurityMode::Sign:
      return 2;
    case MessageSecurityMode::None:
      return 1;
    case MessageSecurityMode::Invalid:
      break;
  }
  return 0;
}

bool IsSupported(const ClientCapabilities& capabilities,
                 const EndpointDescription& endpoint) {
  return std::ranges::any_of(
      capabilities.supported, [&](const SupportedSecurity& supported) {
        return supported.security_policy_uri == endpoint.security_policy_uri &&
               supported.security_mode == endpoint.security_mode;
      });
}

bool MatchesPreference(const SecurityPreference& preference,
                       const EndpointDescription& endpoint) {
  switch (preference.mode) {
    case SecurityPreference::Mode::None:
      if (endpoint.security_mode != MessageSecurityMode::None) {
        return false;
      }
      break;
    case SecurityPreference::Mode::SignAndEncrypt:
      if (endpoint.security_mode != MessageSecurityMode::SignAndEncrypt) {
        return false;
      }
      break;
    case SecurityPreference::Mode::Auto:
      break;
  }
  return !preference.required_policy_uri ||
         *preference.required_policy_uri == endpoint.security_policy_uri;
}

// Strict ordering used to pick the "best" endpoint: more secure mode first,
// then the server's advertised securityLevel. The first endpoint in iteration
// order wins ties so selection is stable.
bool IsBetter(const EndpointDescription& candidate,
              const EndpointDescription& incumbent) {
  const int candidate_rank = SecurityRank(candidate.security_mode);
  const int incumbent_rank = SecurityRank(incumbent.security_mode);
  if (candidate_rank != incumbent_rank) {
    return candidate_rank > incumbent_rank;
  }
  return candidate.security_level > incumbent.security_level;
}

}  // namespace

ClientCapabilities ClientCapabilities::Default() {
  return ClientCapabilities{
      .supported = {
          SupportedSecurity{
              .security_policy_uri =
                  std::string{kSecurityPolicyUriBasic256Sha256},
              .security_mode = MessageSecurityMode::SignAndEncrypt},
          SupportedSecurity{
              .security_policy_uri = std::string{kSecurityPolicyUriNone},
              .security_mode = MessageSecurityMode::None},
      }};
}

ClientCapabilities ClientCapabilities::NoneOnly() {
  return ClientCapabilities{
      .supported = {
          SupportedSecurity{
              .security_policy_uri = std::string{kSecurityPolicyUriNone},
              .security_mode = MessageSecurityMode::None},
      }};
}

StatusOr<EndpointDescription> SelectEndpoint(
    std::span<const EndpointDescription> endpoints,
    const SecurityPreference& preference,
    const ClientCapabilities& capabilities) {
  const EndpointDescription* best = nullptr;
  for (const auto& endpoint : endpoints) {
    if (!IsSupported(capabilities, endpoint) ||
        !MatchesPreference(preference, endpoint)) {
      continue;
    }
    if (!best || IsBetter(endpoint, *best)) {
      best = &endpoint;
    }
  }
  if (!best) {
    return StatusOr<EndpointDescription>{Status{StatusCode::Bad}};
  }
  return StatusOr<EndpointDescription>{*best};
}

}  // namespace opcua
