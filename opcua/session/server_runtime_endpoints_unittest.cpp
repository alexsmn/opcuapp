#include "opcua/session/server_runtime.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/session/server_session_manager.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace opcua {
namespace {

constexpr std::string_view kTcpProfile =
    "http://opcfoundation.org/UA-Profile/Transport/uatcp-uasc-uabinary";
constexpr std::string_view kWssProfile =
    "http://opcfoundation.org/UA-Profile/Transport/wss-uajson";

// Discovery-only fixture: GetEndpoints and FindServers are answered before any
// session exists, so the runtime needs no data services and no authenticator.
class ServerRuntimeEndpointsTest : public testing::Test {
 public:
  ServerRuntime MakeRuntime(std::vector<EndpointDescription> endpoints,
                            std::vector<RegisteredServer> registered = {}) {
    return ServerRuntime{ServerRuntimeContext{
        .executor = any_executor_,
        .session_manager = session_manager_,
        .endpoints = std::move(endpoints),
        .registered_servers =
            [registered = std::move(registered)] { return registered; },
    }};
  }

  std::vector<EndpointDescription> GetEndpoints(ServerRuntime& runtime,
                                                std::string dialled_url) {
    ConnectionState connection;
    const auto body = WaitAwaitable(
        executor_,
        runtime.Handle(connection,
                       RequestBody{GetEndpointsRequest{
                           .endpoint_url = std::move(dialled_url)}}));
    const auto* response = std::get_if<GetEndpointsResponse>(&body);
    EXPECT_TRUE(response);
    return response ? response->endpoints : std::vector<EndpointDescription>{};
  }

  std::vector<ApplicationDescription> FindServers(ServerRuntime& runtime,
                                                  std::string dialled_url) {
    ConnectionState connection;
    const auto body = WaitAwaitable(
        executor_,
        runtime.Handle(connection,
                       RequestBody{FindServersRequest{
                           .endpoint_url = std::move(dialled_url)}}));
    const auto* response = std::get_if<FindServersResponse>(&body);
    EXPECT_TRUE(response);
    return response ? response->servers : std::vector<ApplicationDescription>{};
  }

  // The URL of the single endpoint served over `profile`.
  std::string UrlFor(const std::vector<EndpointDescription>& endpoints,
                     std::string_view profile) {
    for (const auto& endpoint : endpoints) {
      if (endpoint.transport_profile_uri == profile)
        return endpoint.endpoint_url;
    }
    ADD_FAILURE() << "no endpoint for transport profile " << profile;
    return {};
  }

  TestExecutor executor_;
  const AnyExecutor any_executor_ = executor_;
  ServerSessionManager session_manager_{ServerSessionManagerContext{}};
};

EndpointDescription TcpEndpoint(std::string url) {
  return {.endpoint_url = std::move(url),
          .transport_profile_uri = std::string{kTcpProfile}};
}

EndpointDescription WssEndpoint(std::string url) {
  return {.endpoint_url = std::move(url),
          .transport_profile_uri = std::string{kWssProfile}};
}

TEST_F(ServerRuntimeEndpointsTest, EchoesDialledUrlForMatchingScheme) {
  auto runtime = MakeRuntime({TcpEndpoint("opc.tcp://0.0.0.0:4840")});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://gateway:4840");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints[0].endpoint_url, "opc.tcp://gateway:4840");
}

// The reported defect: a WSS listener bound to the 0.0.0.0 wildcard was
// advertised verbatim to a client that dialled over TCP, handing it a URL no
// client can connect to.
TEST_F(ServerRuntimeEndpointsTest, RebasesWildcardEndpointOfAnotherScheme) {
  auto runtime = MakeRuntime({TcpEndpoint("opc.tcp://0.0.0.0:4840"),
                              WssEndpoint("opc.wss://0.0.0.0:4843")});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://gateway:4840");

  ASSERT_EQ(endpoints.size(), 2u);
  EXPECT_EQ(UrlFor(endpoints, kTcpProfile), "opc.tcp://gateway:4840");
  EXPECT_EQ(UrlFor(endpoints, kWssProfile), "opc.wss://gateway:4843");
}

TEST_F(ServerRuntimeEndpointsTest, RebasingKeepsConfiguredPortAndPath) {
  auto runtime = MakeRuntime({WssEndpoint("opc.wss://0.0.0.0:4843/ua")});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://gateway:4840");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints[0].endpoint_url, "opc.wss://gateway:4843/ua");
}

TEST_F(ServerRuntimeEndpointsTest, RebasesIpv6Wildcard) {
  auto runtime = MakeRuntime({WssEndpoint("opc.wss://[::]:4843")});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://[2001:db8::1]:4840");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints[0].endpoint_url, "opc.wss://[2001:db8::1]:4843");
}

// An endpoint configured with a routable host — what `advertise_url` produces
// for a listener published through a reverse proxy on another host, port and
// path — is authoritative and must survive a request on another transport.
TEST_F(ServerRuntimeEndpointsTest, KeepsConfiguredHostOfAnotherScheme) {
  auto runtime = MakeRuntime({TcpEndpoint("opc.tcp://0.0.0.0:4840"),
                              WssEndpoint("opc.wss://demo.example/ua")});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://gateway:4840");

  ASSERT_EQ(endpoints.size(), 2u);
  EXPECT_EQ(UrlFor(endpoints, kWssProfile), "opc.wss://demo.example/ua");
}

// The invariant, over every wildcard spelling a listener binds to: whatever the
// configuration says, a wildcard address never reaches a client — not as an
// EndpointUrl, and not as a discovery URL. RFC 3986 requires an IPv6 literal to
// be bracketed once a port follows, so the unbracketed forms appear portless.
TEST_F(ServerRuntimeEndpointsTest, NeverAdvertisesAWildcardHost) {
  const std::pair<std::string, std::string> kWildcardBinds[] = {
      {"opc.wss://0.0.0.0:4843", "opc.wss://gateway:4843"},
      {"opc.wss://[::]:4843", "opc.wss://gateway:4843"},
      {"opc.wss://[::0]:4843", "opc.wss://gateway:4843"},
      {"opc.wss://::", "opc.wss://gateway"},
      {"opc.wss://0:0:0:0:0:0:0:0", "opc.wss://gateway"},
      {"opc.wss://0.0.0.0:4843/ua", "opc.wss://gateway:4843/ua"},
  };

  for (const auto& [bind_url, expected] : kWildcardBinds) {
    auto runtime = MakeRuntime(
        {TcpEndpoint("opc.tcp://0.0.0.0:4840"), WssEndpoint(bind_url)},
        {RegisteredServer{.server_uri = "urn:edge",
                          .discovery_urls = {bind_url}}});

    EXPECT_EQ(
        UrlFor(GetEndpoints(runtime, "opc.tcp://gateway:4840"), kWssProfile),
        expected)
        << "bind URL " << bind_url << " leaked into an advertised EndpointUrl";

    const auto servers = FindServers(runtime, "opc.tcp://gateway:4840");
    const auto edge = std::ranges::find(
        servers, "urn:edge", &ApplicationDescription::application_uri);
    ASSERT_NE(edge, servers.end());
    EXPECT_THAT(edge->discovery_urls, testing::ElementsAre(expected))
        << "bind URL " << bind_url << " leaked into a discovery URL";
  }
}

TEST_F(ServerRuntimeEndpointsTest, RebasesDiscoveryUrlsOfTheEmbeddedServer) {
  auto endpoint = WssEndpoint("opc.wss://0.0.0.0:4843");
  endpoint.server.application_uri = "urn:proxy";
  endpoint.server.discovery_urls = {"opc.wss://0.0.0.0:4843"};
  auto runtime = MakeRuntime({std::move(endpoint)});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://gateway:4840");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_THAT(endpoints[0].server.discovery_urls,
              testing::ElementsAre("opc.wss://gateway:4843"));
}

TEST_F(ServerRuntimeEndpointsTest, RebasesRegisteredServerDiscoveryUrls) {
  auto runtime = MakeRuntime(
      {TcpEndpoint("opc.tcp://0.0.0.0:4840")},
      {RegisteredServer{.server_uri = "urn:historian",
                        .discovery_urls = {"opc.tcp://0.0.0.0:4842"}}});

  const auto servers = FindServers(runtime, "opc.tcp://gateway:4840");

  const auto historian = std::ranges::find(
      servers, "urn:historian", &ApplicationDescription::application_uri);
  ASSERT_NE(historian, servers.end());
  EXPECT_THAT(historian->discovery_urls,
              testing::ElementsAre("opc.tcp://gateway:4842"));
}

// Nothing to rebase onto: a request that carries no endpointUrl (permitted —
// OPC UA Part 4 §5.4.4 leaves it optional) leaves the configuration alone
// rather than inventing a host.
TEST_F(ServerRuntimeEndpointsTest, LeavesEndpointsAloneWithoutARequestUrl) {
  auto runtime = MakeRuntime({TcpEndpoint("opc.tcp://0.0.0.0:4840")});

  const auto endpoints = GetEndpoints(runtime, "");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints[0].endpoint_url, "opc.tcp://0.0.0.0:4840");
}

// A client that itself dialled a wildcard (a loopback-local tool) gives the
// server nothing routable to echo, so the configuration stands.
TEST_F(ServerRuntimeEndpointsTest, IgnoresAWildcardRequestUrl) {
  auto runtime = MakeRuntime({TcpEndpoint("opc.tcp://proxy:4840")});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://0.0.0.0:4840");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints[0].endpoint_url, "opc.tcp://proxy:4840");
}

}  // namespace
}  // namespace opcua
