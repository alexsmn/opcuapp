#include "opcua/client/client_security.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

constexpr std::string_view kNone =
    "http://opcfoundation.org/UA/SecurityPolicy#None";
constexpr std::string_view kBasic256Sha256 =
    "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256";
constexpr std::string_view kTcpProfile =
    "http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary";

// ByteString is std::vector<char>, so a string literal will not do.
ByteString Bytes(std::string_view text) {
  return ByteString(text.begin(), text.end());
}

EndpointDescription MakeSecured(std::string url = "opc.tcp://host:4840") {
  return EndpointDescription{
      .endpoint_url = std::move(url),
      .server_certificate = Bytes("server-der"),
      .security_mode = MessageSecurityMode::SignAndEncrypt,
      .security_policy_uri = std::string{kBasic256Sha256},
      .user_identity_tokens =
          {
              UserTokenPolicy{.policy_id = "anonymous",
                              .token_type = UserTokenType::Anonymous},
              UserTokenPolicy{
                  .policy_id = "username",
                  .token_type = UserTokenType::UserName,
                  .security_policy_uri = std::string{kBasic256Sha256}},
          },
      .transport_profile_uri = std::string{kTcpProfile},
  };
}

EndpointDescription MakeUnsecured(std::string url = "opc.tcp://host:4840") {
  return EndpointDescription{
      .endpoint_url = std::move(url),
      .server_certificate = Bytes("server-der"),
      .security_mode = MessageSecurityMode::None,
      .security_policy_uri = std::string{kNone},
      .user_identity_tokens =
          {
              UserTokenPolicy{.policy_id = "anonymous",
                              .token_type = UserTokenType::Anonymous},
          },
      .transport_profile_uri = std::string{kTcpProfile},
  };
}

// The whole point of ignoring endpointUrl: the server rewrites it per caller,
// so a comparison that included it would reject every honest connection.
TEST(SecurityEquivalentTest, IgnoresTheEndpointUrl) {
  const auto discovered = MakeSecured("opc.tcp://127.0.0.1:14840");
  const auto authoritative = MakeSecured("opc.tcp://proxy:4840");
  EXPECT_TRUE(SecurityEquivalent(discovered, authoritative));
}

TEST(SecurityEquivalentTest, DetectsADowngradedSecurityMode) {
  EXPECT_FALSE(SecurityEquivalent(MakeSecured(), MakeUnsecured()));
}

TEST(SecurityEquivalentTest, DetectsASubstitutedServerCertificate) {
  auto tampered = MakeSecured();
  tampered.server_certificate = Bytes("attacker-der");
  EXPECT_FALSE(SecurityEquivalent(MakeSecured(), tampered));
}

// A MITM that leaves mode and certificate alone but weakens the token policy
// would otherwise get the password protected by a policy of its choosing.
TEST(SecurityEquivalentTest, DetectsAWeakenedUserTokenPolicy) {
  auto tampered = MakeSecured();
  tampered.user_identity_tokens[1].security_policy_uri = std::string{kNone};
  EXPECT_FALSE(SecurityEquivalent(MakeSecured(), tampered));
}

TEST(SecurityEquivalentTest, DetectsARemovedUserTokenPolicy) {
  auto tampered = MakeSecured();
  tampered.user_identity_tokens.pop_back();
  EXPECT_FALSE(SecurityEquivalent(MakeSecured(), tampered));
}

// A server predating CreateSession.serverEndpoints sends nothing; failing
// closed there would break every connection to one.
TEST(EndpointSetsSecurityEquivalentTest, EmptyAuthoritativeListPasses) {
  const std::vector<EndpointDescription> discovered = {MakeSecured()};
  EXPECT_TRUE(EndpointSetsSecurityEquivalent(discovered, {}));
}

TEST(EndpointSetsSecurityEquivalentTest, MatchesRegardlessOfOrder) {
  const std::vector<EndpointDescription> discovered = {MakeUnsecured(),
                                                       MakeSecured()};
  const std::vector<EndpointDescription> authoritative = {MakeSecured(),
                                                          MakeUnsecured()};
  EXPECT_TRUE(EndpointSetsSecurityEquivalent(discovered, authoritative));
}

// The downgrade this check exists for: discovery advertised only the unsecured
// endpoint, but the server actually offers both.
TEST(EndpointSetsSecurityEquivalentTest, DetectsAStrippedSecuredEndpoint) {
  const std::vector<EndpointDescription> discovered = {MakeUnsecured()};
  const std::vector<EndpointDescription> authoritative = {MakeSecured(),
                                                          MakeUnsecured()};
  EXPECT_FALSE(EndpointSetsSecurityEquivalent(discovered, authoritative));
}

TEST(EndpointSetsSecurityEquivalentTest, DetectsAnInjectedEndpoint) {
  const std::vector<EndpointDescription> discovered = {MakeSecured(),
                                                       MakeUnsecured()};
  const std::vector<EndpointDescription> authoritative = {MakeSecured()};
  EXPECT_FALSE(EndpointSetsSecurityEquivalent(discovered, authoritative));
}

// The regression this guards, and it took down every tier link the moment the
// server learned to send serverEndpoints: with Mode::None — the default, and
// every link configured without a "security" block — ClientSession never runs
// GetEndpoints, so it has nothing to compare. A size check alone reads that as
// a stripped-endpoint attack and rejects the session between CreateSession and
// ActivateSession, which is a connection that worked yesterday failing today
// for a reason no log explains.
TEST(EndpointSetsSecurityEquivalentTest, NoDiscoveryMeansNothingToRecheck) {
  const std::vector<EndpointDescription> authoritative = {MakeSecured(),
                                                          MakeUnsecured()};
  EXPECT_TRUE(EndpointSetsSecurityEquivalent({}, authoritative));
}

// Same count, same shapes, but one entry differs — the pairing must not be
// fooled by matching an already-consumed counterpart twice.
TEST(EndpointSetsSecurityEquivalentTest, DoesNotReuseOneMatchTwice) {
  const std::vector<EndpointDescription> discovered = {MakeSecured(),
                                                       MakeSecured()};
  const std::vector<EndpointDescription> authoritative = {MakeSecured(),
                                                          MakeUnsecured()};
  EXPECT_FALSE(EndpointSetsSecurityEquivalent(discovered, authoritative));
}

}  // namespace
}  // namespace opcua
