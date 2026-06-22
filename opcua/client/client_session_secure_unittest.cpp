#include "opcua/client/client_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time/time.h"
#include "opcua/client/endpoint_selection.h"
#include "opcua/session/session_types.h"
#include "opcua/test/scripted_transport.h"
#include "opcua/types/node_id.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace opcua {
namespace {

EndpointDescription NoneEndpoint() {
  return EndpointDescription{
      .endpoint_url = "opc.tcp://localhost:4840",
      .security_mode = MessageSecurityMode::None,
      .security_policy_uri = std::string{kSecurityPolicyUriNone},
  };
}

std::shared_ptr<test::ScriptedState> MakeDiscoveryScript(
    std::vector<EndpointDescription> endpoints) {
  auto state = std::make_shared<test::ScriptedState>();
  test::PrimeConnectAndOpen(state);
  state->incoming.push_back(test::AsString(test::BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/2,
      ResponseBody{GetEndpointsResponse{.status = opcua::StatusCode::Good,
                                        .endpoints = std::move(endpoints)}})));
  return state;
}

std::shared_ptr<test::ScriptedState> MakeSessionScript() {
  auto state = std::make_shared<test::ScriptedState>();
  test::PrimeConnectAndOpen(state);
  state->incoming.push_back(test::AsString(test::BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSessionResponse{
          .status = opcua::StatusCode::Good,
          .session_id = opcua::NodeId{111},
          .authentication_token = opcua::NodeId{222},
          .server_nonce = opcua::ByteString{},
          .revised_timeout = opcua::base::TimeDelta::FromSeconds(60)}})));
  state->incoming.push_back(test::AsString(test::BuildServiceResponseFrame(
      /*request_id=*/3, /*request_handle=*/2,
      ResponseBody{
          ActivateSessionResponse{.status = opcua::StatusCode::Good}})));
  return state;
}

bool ContainsRequest(const std::vector<std::string>& writes, auto predicate) {
  const auto requests = test::DecodeServiceRequests(writes);
  return std::ranges::find_if(requests, predicate) != requests.end();
}

TEST(ClientSessionSecureTest, AutoModeDiscoversNoneEndpointThenConnects) {
  auto discovery_state = MakeDiscoveryScript({NoneEndpoint()});
  auto session_state = MakeSessionScript();

  opcua::TestExecutor executor;
  test::ScriptedTransportFactory factory{
      std::vector<std::shared_ptr<test::ScriptedState>>{discovery_state,
                                                        session_state}};
  auto session = std::make_shared<ClientSession>(executor, factory);

  opcua::SessionConnectParams params;
  params.host = "localhost:4840";
  params.security.mode = opcua::SessionSecuritySettings::Mode::Auto;

  auto status = opcua::WaitAwaitable(executor, session->ConnectStatus(params));

  EXPECT_EQ(status.code(), opcua::StatusCode::Good);
  EXPECT_TRUE(session->IsConnected());

  // Discovery ran over its own connection, and the working session opened
  // afterwards.
  EXPECT_TRUE(
      ContainsRequest(discovery_state->writes, [](const RequestBody& body) {
        return std::holds_alternative<GetEndpointsRequest>(body);
      }));
  EXPECT_TRUE(
      ContainsRequest(session_state->writes, [](const RequestBody& body) {
        return std::holds_alternative<CreateSessionRequest>(body);
      }));
}

TEST(ClientSessionSecureTest, SignAndEncryptRejectedWhenServerOffersOnlyNone) {
  // Discovery succeeds but the only endpoint is unsecured, so selection fails
  // and no working session connection is attempted.
  auto discovery_state = MakeDiscoveryScript({NoneEndpoint()});

  opcua::TestExecutor executor;
  test::ScriptedTransportFactory factory{discovery_state};
  auto session = std::make_shared<ClientSession>(executor, factory);

  opcua::SessionConnectParams params;
  params.host = "localhost:4840";
  params.security.mode = opcua::SessionSecuritySettings::Mode::SignAndEncrypt;

  auto status = opcua::WaitAwaitable(executor, session->ConnectStatus(params));

  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(session->IsConnected());
}

TEST(ClientSessionSecureTest, DefaultModeSkipsDiscovery) {
  // With the default (Mode::None) the session connects directly; no
  // GetEndpoints is ever sent.
  auto session_state = MakeSessionScript();

  opcua::TestExecutor executor;
  test::ScriptedTransportFactory factory{session_state};
  auto session = std::make_shared<ClientSession>(executor, factory);

  auto status = opcua::WaitAwaitable(
      executor, session->ConnectStatus({.host = "localhost:4840"}));

  EXPECT_EQ(status.code(), opcua::StatusCode::Good);
  EXPECT_FALSE(
      ContainsRequest(session_state->writes, [](const RequestBody& body) {
        return std::holds_alternative<GetEndpointsRequest>(body);
      }));
}

}  // namespace
}  // namespace opcua
