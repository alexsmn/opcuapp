#include "opcua/session/discovery_conversion.h"

#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"

#include <boost/json.hpp>

#include <cstdint>
#include <type_traits>

namespace opcua::discovery_conversion {
namespace {

// JSON DefaultJson encoding id for MdnsDiscoveryConfiguration (NodeIds.csv);
// the DefaultBinary id comes from BinaryEncodingId<T>.
constexpr std::uint32_t kMdnsDiscoveryConfigurationJsonId = 15106;

// The discovery enums (ApplicationType, MessageSecurityMode, UserTokenType) are
// spec enumerations with identical values in both type systems; only the
// underlying integer type differs (hand-written UInt32 vs generated Int32).
template <class To, class From>
To CastEnum(From value) {
  return static_cast<To>(static_cast<std::underlying_type_t<From>>(value));
}

ua::ApplicationDescription ToUa(const ApplicationDescription& m) {
  return ua::ApplicationDescription{
      .application_uri = m.application_uri,
      .product_uri = m.product_uri,
      .application_name = m.application_name,
      .application_type = CastEnum<ua::ApplicationType>(m.application_type),
      .gateway_server_uri = m.gateway_server_uri,
      .discovery_profile_uri = m.discovery_profile_uri,
      .discovery_urls = m.discovery_urls,
  };
}

ApplicationDescription FromUa(const ua::ApplicationDescription& w) {
  return ApplicationDescription{
      .application_uri = w.application_uri,
      .product_uri = w.product_uri,
      .application_name = w.application_name,
      .application_type = CastEnum<ApplicationType>(w.application_type),
      .gateway_server_uri = w.gateway_server_uri,
      .discovery_profile_uri = w.discovery_profile_uri,
      .discovery_urls = w.discovery_urls,
  };
}

ua::UserTokenPolicy ToUa(const UserTokenPolicy& m) {
  return ua::UserTokenPolicy{
      .policy_id = m.policy_id,
      .token_type = CastEnum<ua::UserTokenType>(m.token_type),
      .issued_token_type = m.issued_token_type,
      .issuer_endpoint_url = m.issuer_endpoint_url,
      .security_policy_uri = m.security_policy_uri,
  };
}

UserTokenPolicy FromUa(const ua::UserTokenPolicy& w) {
  return UserTokenPolicy{
      .policy_id = w.policy_id,
      .token_type = CastEnum<UserTokenType>(w.token_type),
      .issued_token_type = w.issued_token_type,
      .issuer_endpoint_url = w.issuer_endpoint_url,
      .security_policy_uri = w.security_policy_uri,
  };
}

ua::EndpointDescription ToUa(const EndpointDescription& m) {
  ua::EndpointDescription w;
  w.endpoint_url = m.endpoint_url;
  w.server = ToUa(m.server);
  w.server_certificate = m.server_certificate;
  w.security_mode = CastEnum<ua::MessageSecurityMode>(m.security_mode);
  w.security_policy_uri = m.security_policy_uri;
  w.user_identity_tokens.reserve(m.user_identity_tokens.size());
  for (const auto& policy : m.user_identity_tokens) {
    w.user_identity_tokens.push_back(ToUa(policy));
  }
  w.transport_profile_uri = m.transport_profile_uri;
  w.security_level = m.security_level;
  return w;
}

EndpointDescription FromUa(const ua::EndpointDescription& w) {
  EndpointDescription m;
  m.endpoint_url = w.endpoint_url;
  m.server = FromUa(w.server);
  m.server_certificate = w.server_certificate;
  m.security_mode = CastEnum<MessageSecurityMode>(w.security_mode);
  m.security_policy_uri = w.security_policy_uri;
  m.user_identity_tokens.reserve(w.user_identity_tokens.size());
  for (const auto& policy : w.user_identity_tokens) {
    m.user_identity_tokens.push_back(FromUa(policy));
  }
  m.transport_profile_uri = w.transport_profile_uri;
  m.security_level = w.security_level;
  return m;
}

ua::RegisteredServer ToUa(const RegisteredServer& m) {
  ua::RegisteredServer w;
  w.server_uri = m.server_uri;
  w.product_uri = m.product_uri;
  w.server_names = m.server_names;
  w.server_type = CastEnum<ua::ApplicationType>(m.server_type);
  w.gateway_server_uri = m.gateway_server_uri;
  w.discovery_urls = m.discovery_urls;
  w.semaphore_file_path = m.semaphore_file_path;
  w.is_online = m.is_online;
  return w;
}

RegisteredServer FromUa(const ua::RegisteredServer& w) {
  RegisteredServer m;
  m.server_uri = w.server_uri;
  m.product_uri = w.product_uri;
  m.server_names = w.server_names;
  m.server_type = CastEnum<ApplicationType>(w.server_type);
  m.gateway_server_uri = w.gateway_server_uri;
  m.discovery_urls = w.discovery_urls;
  m.semaphore_file_path = w.semaphore_file_path;
  m.is_online = w.is_online;
  return m;
}

// Decodes one discoveryConfiguration ExtensionObject, accepting the binary body
// (opc.tcp / a JSON body tagged UaEncoding=1) and the inline JSON body form.
// Returns nullopt for any extension type other than MdnsDiscoveryConfiguration,
// preserving the entry so the handler answers Bad_NotSupported for it by index.
std::optional<MdnsDiscoveryConfiguration> DecodeDiscoveryConfiguration(
    const ExtensionObject& configuration) {
  ua::MdnsDiscoveryConfiguration mdns;
  if (ua::FromExtensionObject(configuration, mdns)) {
    return MdnsDiscoveryConfiguration{
        .mdns_server_name = mdns.mdns_server_name,
        .server_capabilities = mdns.server_capabilities};
  }
  const auto* json = std::any_cast<boost::json::value>(&configuration.value());
  if (json == nullptr) {
    return std::nullopt;
  }
  const NodeId& id = configuration.data_type_id().node_id();
  const bool is_mdns =
      id.is_numeric() && id.namespace_index() == 0 &&
      (id.numeric_id() ==
           ua::BinaryEncodingId<ua::MdnsDiscoveryConfiguration>::value ||
       id.numeric_id() == kMdnsDiscoveryConfigurationJsonId);
  if (!is_mdns) {
    return std::nullopt;
  }
  ua::DecodeJson(*json, mdns);
  return MdnsDiscoveryConfiguration{
      .mdns_server_name = mdns.mdns_server_name,
      .server_capabilities = mdns.server_capabilities};
}

}  // namespace

FindServersRequest ToManaged(const ua::FindServersRequest& wire) {
  return FindServersRequest{
      .endpoint_url = wire.endpoint_url,
      .locale_ids = wire.locale_ids,
      .server_uris = wire.server_uris,
  };
}

GetEndpointsRequest ToManaged(const ua::GetEndpointsRequest& wire) {
  return GetEndpointsRequest{
      .endpoint_url = wire.endpoint_url,
      .locale_ids = wire.locale_ids,
      .profile_uris = wire.profile_uris,
  };
}

RegisterServerRequest ToManaged(const ua::RegisterServerRequest& wire) {
  return RegisterServerRequest{.server = FromUa(wire.server)};
}

RegisterServer2Request ToManaged(const ua::RegisterServer2Request& wire) {
  RegisterServer2Request request{.server = FromUa(wire.server)};
  request.discovery_configuration.reserve(wire.discovery_configuration.size());
  for (const auto& configuration : wire.discovery_configuration) {
    request.discovery_configuration.push_back(
        DecodeDiscoveryConfiguration(configuration));
  }
  return request;
}

ua::FindServersResponse ToWire(const FindServersResponse& managed) {
  ua::FindServersResponse wire;
  wire.response_header.service_result = managed.status;
  wire.servers.reserve(managed.servers.size());
  for (const auto& server : managed.servers) {
    wire.servers.push_back(ToUa(server));
  }
  return wire;
}

ua::GetEndpointsResponse ToWire(const GetEndpointsResponse& managed) {
  ua::GetEndpointsResponse wire;
  wire.response_header.service_result = managed.status;
  wire.endpoints.reserve(managed.endpoints.size());
  for (const auto& endpoint : managed.endpoints) {
    wire.endpoints.push_back(ToUa(endpoint));
  }
  return wire;
}

ua::RegisterServerResponse ToWire(const RegisterServerResponse& managed) {
  ua::RegisterServerResponse wire;
  wire.response_header.service_result = managed.status;
  return wire;
}

ua::RegisterServer2Response ToWire(const RegisterServer2Response& managed) {
  ua::RegisterServer2Response wire;
  wire.response_header.service_result = managed.status;
  wire.configuration_results.reserve(managed.configuration_results.size());
  for (const auto result : managed.configuration_results) {
    wire.configuration_results.push_back(Status{result});
  }
  return wire;
}

ua::FindServersRequest ToWire(const FindServersRequest& managed) {
  ua::FindServersRequest wire;
  wire.endpoint_url = managed.endpoint_url;
  wire.locale_ids = managed.locale_ids;
  wire.server_uris = managed.server_uris;
  return wire;
}

ua::GetEndpointsRequest ToWire(const GetEndpointsRequest& managed) {
  ua::GetEndpointsRequest wire;
  wire.endpoint_url = managed.endpoint_url;
  wire.locale_ids = managed.locale_ids;
  wire.profile_uris = managed.profile_uris;
  return wire;
}

ua::RegisterServerRequest ToWire(const RegisterServerRequest& managed) {
  ua::RegisterServerRequest wire;
  wire.server = ToUa(managed.server);
  return wire;
}

ua::RegisterServer2Request ToWire(const RegisterServer2Request& managed) {
  ua::RegisterServer2Request wire;
  wire.server = ToUa(managed.server);
  // A client only sends the entries it supports; unsupported (nullopt) entries
  // are decode-side placeholders and are not re-encoded.
  for (const auto& configuration : managed.discovery_configuration) {
    if (configuration.has_value()) {
      wire.discovery_configuration.push_back(
          ua::ToExtensionObject(ua::MdnsDiscoveryConfiguration{
              .mdns_server_name = configuration->mdns_server_name,
              .server_capabilities = configuration->server_capabilities}));
    }
  }
  return wire;
}

FindServersResponse ToManaged(const ua::FindServersResponse& wire) {
  FindServersResponse managed{.status = wire.response_header.service_result};
  managed.servers.reserve(wire.servers.size());
  for (const auto& server : wire.servers) {
    managed.servers.push_back(FromUa(server));
  }
  return managed;
}

GetEndpointsResponse ToManaged(const ua::GetEndpointsResponse& wire) {
  GetEndpointsResponse managed{.status = wire.response_header.service_result};
  managed.endpoints.reserve(wire.endpoints.size());
  for (const auto& endpoint : wire.endpoints) {
    managed.endpoints.push_back(FromUa(endpoint));
  }
  return managed;
}

RegisterServerResponse ToManaged(const ua::RegisterServerResponse& wire) {
  return RegisterServerResponse{.status = wire.response_header.service_result};
}

RegisterServer2Response ToManaged(const ua::RegisterServer2Response& wire) {
  RegisterServer2Response managed{.status =
                                      wire.response_header.service_result};
  managed.configuration_results.reserve(wire.configuration_results.size());
  for (const auto& result : wire.configuration_results) {
    managed.configuration_results.push_back(result.code());
  }
  return managed;
}

}  // namespace opcua::discovery_conversion
