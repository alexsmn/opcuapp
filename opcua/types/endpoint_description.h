#pragma once

// The descriptor types a Server publishes about itself and its Endpoints (OPC
// UA Part 4 §7 Common Parameter Type Definitions). They live below the service
// messages because both discovery (FindServers, GetEndpoints) and session
// establishment (CreateSession serverEndpoints, Part 4 §5.6.2) carry them.

#include "opcua/types/basic_types.h"
#include "opcua/types/localized_text.h"

#include <string>
#include <vector>

namespace opcua {

// The type of an OPC UA application (Server, Client, both, or DiscoveryServer).
// OPC UA Part 4 §7.4 ApplicationType,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.4
enum class ApplicationType : UInt32 {
  Server = 0,
  Client = 1,
  ClientAndServer = 2,
  DiscoveryServer = 3,
};

// The kind of UserIdentityToken a Server Endpoint accepts. OPC UA Part 4 §7.42
// UserTokenType, https://reference.opcfoundation.org/Core/Part4/v105/docs/7.42
enum class UserTokenType : UInt32 {
  Anonymous = 0,
  UserName = 1,
  Certificate = 2,
  IssuedToken = 3,
};

// Security mode applied to a SecureChannel (None, Sign, or SignAndEncrypt). OPC
// UA Part 4 §7.20 MessageSecurityMode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.20
enum class MessageSecurityMode : UInt32 {
  Invalid = 0,
  None = 1,
  Sign = 2,
  SignAndEncrypt = 3,
};

// Describes an OPC UA application returned by FindServers. OPC UA Part 4 §7.2
// ApplicationDescription,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.2
struct ApplicationDescription {
  std::string application_uri;
  std::string product_uri;
  LocalizedText application_name;
  ApplicationType application_type = ApplicationType::Server;
  std::string gateway_server_uri;
  std::string discovery_profile_uri;
  std::vector<std::string> discovery_urls;
};

// Specifies a UserIdentityToken that a Server Endpoint accepts. OPC UA Part 4
// §7.41 UserTokenPolicy,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.41
struct UserTokenPolicy {
  std::string policy_id;
  UserTokenType token_type = UserTokenType::Anonymous;
  std::string issued_token_type;
  std::string issuer_endpoint_url;
  std::string security_policy_uri;
};

// Describes an Endpoint that a Server exposes (URL, security, accepted tokens).
// OPC UA Part 4 §7.14 EndpointDescription,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.14
struct EndpointDescription {
  std::string endpoint_url;
  ApplicationDescription server;
  ByteString server_certificate;
  MessageSecurityMode security_mode = MessageSecurityMode::None;
  std::string security_policy_uri;
  std::vector<UserTokenPolicy> user_identity_tokens;
  std::string transport_profile_uri;
  UInt8 security_level = 0;
};

}  // namespace opcua
