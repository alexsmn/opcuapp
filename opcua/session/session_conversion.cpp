#include "opcua/session/session_conversion.h"

#include "opcua/session/discovery_conversion.h"
#include "opcua/types/localized_text.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"

#include <boost/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace opcua::session_conversion {
namespace {

// JSON DefaultJson encoding ids for the identity tokens (schema NodeIds.csv).
// A JSON-envelope client (the web client, which has no binary encoder) inlines
// the token under this id; the binary DefaultBinary ids come from
// BinaryEncodingId<T>.
constexpr std::uint32_t kAnonymousIdentityTokenJsonId = 15141;
constexpr std::uint32_t kUserNameIdentityTokenJsonId = 15142;

// The EndpointDescriptions CreateSession returns in `serverEndpoints` are the
// GetEndpoints descriptors (OPC UA Part 4 §5.6.2), mapped by the discovery
// codec seam so the two services cannot drift apart.
std::vector<EndpointDescription> FromUaEndpoints(
    std::span<const ua::EndpointDescription> wire) {
  std::vector<EndpointDescription> endpoints;
  endpoints.reserve(wire.size());
  for (const auto& endpoint : wire) {
    endpoints.push_back(discovery_conversion::FromUa(endpoint));
  }
  return endpoints;
}

ByteString ToByteString(const String& text) {
  return ByteString{text.begin(), text.end()};
}

std::string FromByteString(const ByteString& bytes) {
  return std::string{bytes.begin(), bytes.end()};
}

// Matches an ExtensionObject's type id against a token's binary or JSON
// encoding id (a JSON-envelope client tags the inline body with the JSON id).
template <class Token>
bool IsTokenType(const ExtensionObject& token, std::uint32_t json_id) {
  const NodeId& id = token.data_type_id().node_id();
  return id.is_numeric() && id.namespace_index() == 0 &&
         (id.numeric_id() == ua::BinaryEncodingId<Token>::value ||
          id.numeric_id() == json_id);
}

// Fills the managed ActivateSession credential fields from a decoded
// UserNameIdentityToken, routing an encrypted vs. cleartext password exactly as
// the hand-written decoder did (OPC UA Part 4 §7.36.4).
void ApplyUserNameToken(const ua::UserNameIdentityToken& token,
                        ActivateSessionRequest& request) {
  request.user_name = ToLocalizedText(token.user_name);
  if (token.encryption_algorithm.empty()) {
    // Channel-protected (or insecure) token: the password is in the clear.
    request.password = ToLocalizedText(FromByteString(token.password));
  } else {
    // Encrypted token: the manager decrypts it with the server private key.
    request.encrypted_password = token.password;
    request.password_encryption_algorithm = token.encryption_algorithm;
  }
}

// Decodes the UserIdentityToken ExtensionObject into the managed request,
// accepting both the binary body form (opc.tcp / a JSON body tagged
// UaEncoding=1) and the inline JSON body form (a conformant JSON-envelope
// client). Returns false on an unsupported or malformed token.
bool DecodeUserIdentityToken(const ExtensionObject& token,
                             ActivateSessionRequest& request) {
  // Binary body: decode via the schema-derived binary codec.
  ua::AnonymousIdentityToken anonymous;
  if (ua::FromExtensionObject(token, anonymous)) {
    request.allow_anonymous = true;
    return true;
  }
  ua::UserNameIdentityToken user_name;
  if (ua::FromExtensionObject(token, user_name)) {
    ApplyUserNameToken(user_name, request);
    return true;
  }

  // Inline JSON body: decode via the schema-derived JSON codec.
  const auto* json = std::any_cast<boost::json::value>(&token.value());
  if (json == nullptr) {
    return false;
  }
  if (IsTokenType<ua::AnonymousIdentityToken>(token,
                                              kAnonymousIdentityTokenJsonId)) {
    request.allow_anonymous = true;
    return true;
  }
  if (IsTokenType<ua::UserNameIdentityToken>(token,
                                             kUserNameIdentityTokenJsonId)) {
    ua::UserNameIdentityToken decoded;
    ua::DecodeJson(*json, decoded);
    ApplyUserNameToken(decoded, request);
    return true;
  }
  return false;
}

}  // namespace

CreateSessionRequest ToManaged(const ua::CreateSessionRequest& wire) {
  return CreateSessionRequest{
      .requested_timeout =
          Duration::FromMillisecondsD(wire.requested_session_timeout),
      .endpoint_url = wire.endpoint_url,
      .client_certificate = wire.client_certificate,
      .client_nonce = wire.client_nonce,
  };
}

std::optional<ActivateSessionRequest> ToManaged(
    const ua::ActivateSessionRequest& wire) {
  // The session is identified by the numeric authentication token in the
  // request header (OPC UA Part 4 §5.6.3).
  if (!wire.request_header.authentication_token.is_numeric()) {
    return std::nullopt;
  }
  ActivateSessionRequest request{
      .authentication_token = wire.request_header.authentication_token,
      .client_signature_algorithm = wire.client_signature.algorithm,
      .client_signature = wire.client_signature.signature,
  };
  if (!DecodeUserIdentityToken(wire.user_identity_token, request)) {
    return std::nullopt;
  }
  return request;
}

CloseSessionRequest ToManaged(const ua::CloseSessionRequest& wire) {
  return CloseSessionRequest{
      .authentication_token = wire.request_header.authentication_token,
  };
}

ua::CreateSessionResponse ToWire(const CreateSessionResponse& managed) {
  ua::CreateSessionResponse wire;
  wire.response_header.service_result = managed.status;
  wire.session_id = managed.session_id;
  wire.authentication_token = managed.authentication_token;
  wire.revised_session_timeout = managed.revised_timeout.InMillisecondsF();
  wire.server_nonce = managed.server_nonce;
  wire.server_certificate = managed.server_certificate;
  wire.server_endpoints.reserve(managed.server_endpoints.size());
  for (const auto& endpoint : managed.server_endpoints) {
    wire.server_endpoints.push_back(discovery_conversion::ToUa(endpoint));
  }
  return wire;
}

ua::ActivateSessionResponse ToWire(const ActivateSessionResponse& managed) {
  ua::ActivateSessionResponse wire;
  wire.response_header.service_result = managed.status;
  return wire;
}

ua::CloseSessionResponse ToWire(const CloseSessionResponse& managed) {
  ua::CloseSessionResponse wire;
  wire.response_header.service_result = managed.status;
  return wire;
}

ua::CreateSessionRequest ToWire(const CreateSessionRequest& managed) {
  ua::CreateSessionRequest wire;
  // The URL this client dialled, which the server answers against when it
  // builds `serverEndpoints` (OPC UA Part 4 §5.6.2). A localhost placeholder
  // keeps the request well-formed for a caller that does not track it; the
  // session name is ignored under the in-repo profile.
  wire.endpoint_url = managed.endpoint_url.empty() ? "opc.tcp://localhost:4840"
                                                   : managed.endpoint_url;
  wire.session_name = "binary-session";
  wire.client_nonce = managed.client_nonce;
  wire.client_certificate = managed.client_certificate;
  wire.requested_session_timeout = managed.requested_timeout.InMillisecondsF();
  return wire;
}

ua::ActivateSessionRequest ToWire(const ActivateSessionRequest& managed) {
  ua::ActivateSessionRequest wire;
  wire.client_signature.algorithm = managed.client_signature_algorithm;
  wire.client_signature.signature = managed.client_signature;
  if (managed.allow_anonymous) {
    wire.user_identity_token =
        ua::ToExtensionObject(ua::AnonymousIdentityToken{});
  } else {
    ua::UserNameIdentityToken token;
    if (managed.user_name.has_value()) {
      token.user_name = ToString(*managed.user_name);
    }
    if (managed.password.has_value()) {
      token.password = ToByteString(ToString(*managed.password));
    }
    wire.user_identity_token = ua::ToExtensionObject(token);
  }
  return wire;
}

ua::CloseSessionRequest ToWire(const CloseSessionRequest& managed) {
  ua::CloseSessionRequest wire;
  wire.delete_subscriptions = true;
  return wire;
}

CreateSessionResponse ToManaged(const ua::CreateSessionResponse& wire) {
  return CreateSessionResponse{
      .status = wire.response_header.service_result,
      .session_id = wire.session_id,
      .authentication_token = wire.authentication_token,
      .server_nonce = wire.server_nonce,
      .server_certificate = wire.server_certificate,
      .revised_timeout =
          Duration::FromMillisecondsD(wire.revised_session_timeout),
      .server_endpoints = FromUaEndpoints(wire.server_endpoints),
  };
}

ActivateSessionResponse ToManaged(const ua::ActivateSessionResponse& wire) {
  return ActivateSessionResponse{.status = wire.response_header.service_result};
}

CloseSessionResponse ToManaged(const ua::CloseSessionResponse& wire) {
  return CloseSessionResponse{.status = wire.response_header.service_result};
}

}  // namespace opcua::session_conversion
