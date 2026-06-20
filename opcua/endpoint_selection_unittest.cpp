#include "opcua/endpoint_selection.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace opcua {
namespace {

EndpointDescription MakeEndpoint(std::string policy_uri,
                                 MessageSecurityMode mode,
                                 opcua::scada::UInt8 security_level = 0) {
  return EndpointDescription{
      .endpoint_url = "opc.tcp://host:4840",
      .security_mode = mode,
      .security_policy_uri = std::move(policy_uri),
      .security_level = security_level,
  };
}

EndpointDescription NoneEndpoint(opcua::scada::UInt8 security_level = 0) {
  return MakeEndpoint(std::string{kSecurityPolicyUriNone},
                      MessageSecurityMode::None, security_level);
}

EndpointDescription Basic256SignAndEncryptEndpoint(
    opcua::scada::UInt8 security_level = 1) {
  return MakeEndpoint(std::string{kSecurityPolicyUriBasic256Sha256},
                      MessageSecurityMode::SignAndEncrypt, security_level);
}

SecurityPreference Pref(SecurityPreference::Mode mode) {
  return SecurityPreference{.mode = mode};
}

TEST(SelectEndpointTest, AutoPrefersSignAndEncryptOverNone) {
  const std::vector<EndpointDescription> endpoints = {
      NoneEndpoint(), Basic256SignAndEncryptEndpoint()};

  auto chosen = SelectEndpoint(endpoints, Pref(SecurityPreference::Mode::Auto),
                               ClientCapabilities::Default());

  ASSERT_TRUE(chosen.ok());
  EXPECT_EQ(chosen->security_mode, MessageSecurityMode::SignAndEncrypt);
  EXPECT_EQ(chosen->security_policy_uri,
            std::string{kSecurityPolicyUriBasic256Sha256});
}

TEST(SelectEndpointTest, AutoFallsBackToNoneWhenOnlyNoneOffered) {
  const std::vector<EndpointDescription> endpoints = {NoneEndpoint()};

  auto chosen = SelectEndpoint(endpoints, Pref(SecurityPreference::Mode::Auto),
                               ClientCapabilities::Default());

  ASSERT_TRUE(chosen.ok());
  EXPECT_EQ(chosen->security_mode, MessageSecurityMode::None);
}

TEST(SelectEndpointTest, AutoSkipsPoliciesTheClientCannotSpeak) {
  // The server offers Basic256Sha256/Sign (the client does not implement
  // Sign-only) alongside a usable None endpoint. Selection must skip the
  // unsupported one and fall back to None rather than fail.
  const std::vector<EndpointDescription> endpoints = {
      MakeEndpoint(std::string{kSecurityPolicyUriBasic256Sha256},
                   MessageSecurityMode::Sign, /*security_level=*/5),
      NoneEndpoint()};

  auto chosen = SelectEndpoint(endpoints, Pref(SecurityPreference::Mode::Auto),
                               ClientCapabilities::Default());

  ASSERT_TRUE(chosen.ok());
  EXPECT_EQ(chosen->security_mode, MessageSecurityMode::None);
}

TEST(SelectEndpointTest, NoneModeRejectsWhenOnlySecuredOffered) {
  const std::vector<EndpointDescription> endpoints = {
      Basic256SignAndEncryptEndpoint()};

  auto chosen = SelectEndpoint(endpoints, Pref(SecurityPreference::Mode::None),
                               ClientCapabilities::Default());

  EXPECT_FALSE(chosen.ok());
  EXPECT_TRUE(chosen.status().bad());
}

TEST(SelectEndpointTest, SignAndEncryptModeRejectsWhenOnlyNoneOffered) {
  const std::vector<EndpointDescription> endpoints = {NoneEndpoint()};

  auto chosen =
      SelectEndpoint(endpoints, Pref(SecurityPreference::Mode::SignAndEncrypt),
                     ClientCapabilities::Default());

  EXPECT_FALSE(chosen.ok());
}

TEST(SelectEndpointTest, RequiredPolicyUriNarrowsAutoSelection) {
  // Both endpoints are supported, but the operator pinned SecurityPolicy=None,
  // so the more secure endpoint must not be chosen.
  const std::vector<EndpointDescription> endpoints = {
      Basic256SignAndEncryptEndpoint(), NoneEndpoint()};
  SecurityPreference preference{
      .mode = SecurityPreference::Mode::Auto,
      .required_policy_uri = std::string{kSecurityPolicyUriNone}};

  auto chosen =
      SelectEndpoint(endpoints, preference, ClientCapabilities::Default());

  ASSERT_TRUE(chosen.ok());
  EXPECT_EQ(chosen->security_mode, MessageSecurityMode::None);
}

TEST(SelectEndpointTest, TieBrokenByHigherSecurityLevel) {
  const std::vector<EndpointDescription> endpoints = {
      NoneEndpoint(/*security_level=*/1), NoneEndpoint(/*security_level=*/9)};

  auto chosen = SelectEndpoint(endpoints, Pref(SecurityPreference::Mode::Auto),
                               ClientCapabilities::Default());

  ASSERT_TRUE(chosen.ok());
  EXPECT_EQ(chosen->security_level, 9);
}

TEST(SelectEndpointTest, EmptyEndpointListRejected) {
  auto chosen = SelectEndpoint({}, Pref(SecurityPreference::Mode::Auto),
                               ClientCapabilities::Default());

  EXPECT_FALSE(chosen.ok());
}

}  // namespace
}  // namespace opcua
