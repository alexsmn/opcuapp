#include "opcua/discovery_client.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/endpoint_selection.h"
#include "opcua/test/scripted_transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <variant>

namespace opcua {
namespace {

EndpointDescription NoneEndpoint() {
  return EndpointDescription{
      .endpoint_url = "opc.tcp://localhost:4840",
      .security_mode = MessageSecurityMode::None,
      .security_policy_uri = std::string{kSecurityPolicyUriNone},
  };
}

TEST(DiscoveryClientTest, ReturnsServerEndpointsAndSendsGetEndpoints) {
  auto state = std::make_shared<test::ScriptedState>();
  test::PrimeConnectAndOpen(state);
  // The GetEndpoints request is the first service request after OPN, so it
  // carries request_id 2 (OPN used 1). DiscoveryClient ignores the response's
  // request id, but priming the realistic value keeps the script honest.
  state->incoming.push_back(test::AsString(test::BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/2,
      ResponseBody{GetEndpointsResponse{.status = scada::StatusCode::Good,
                                        .endpoints = {NoneEndpoint()}}})));

  TestExecutor executor;
  test::ScriptedTransportFactory factory{state};
  DiscoveryClient discovery{executor, factory};

  auto result = WaitAwaitable(
      executor, discovery.GetEndpoints("opc.tcp://localhost:4840"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ((*result)[0].security_policy_uri,
            std::string{kSecurityPolicyUriNone});

  const auto requests = test::DecodeServiceRequests(state->writes);
  EXPECT_NE(std::ranges::find_if(
                requests,
                [](const RequestBody& body) {
                  return std::holds_alternative<GetEndpointsRequest>(body);
                }),
            requests.end());
}

TEST(DiscoveryClientTest, PropagatesServiceFaultStatus) {
  auto state = std::make_shared<test::ScriptedState>();
  test::PrimeConnectAndOpen(state);
  state->incoming.push_back(test::AsString(test::BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/2,
      ResponseBody{
          GetEndpointsResponse{.status = scada::StatusCode::Bad_Timeout}})));

  TestExecutor executor;
  test::ScriptedTransportFactory factory{state};
  DiscoveryClient discovery{executor, factory};

  auto result = WaitAwaitable(
      executor, discovery.GetEndpoints("opc.tcp://localhost:4840"));

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), scada::StatusCode::Bad_Timeout);
}

TEST(DiscoveryClientTest, RejectsNonOpcTcpUrl) {
  TestExecutor executor;
  test::ScriptedTransportFactory factory{
      std::make_shared<test::ScriptedState>()};
  DiscoveryClient discovery{executor, factory};

  auto result =
      WaitAwaitable(executor, discovery.GetEndpoints("http://localhost:4840"));

  EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace opcua
