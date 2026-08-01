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

  // The `serverEndpoints` a client retains from CreateSession and reconnects
  // through (OPC UA Part 4 §5.6.2).
  std::vector<EndpointDescription> CreateSessionEndpoints(
      ServerRuntime& runtime,
      std::string dialled_url) {
    ConnectionState connection;
    const auto body = WaitAwaitable(
        executor_,
        runtime.Handle(connection,
                       RequestBody{CreateSessionRequest{
                           .endpoint_url = std::move(dialled_url)}}));
    const auto* response = std::get_if<CreateSessionResponse>(&body);
    EXPECT_TRUE(response);
    return response ? response->server_endpoints
                    : std::vector<EndpointDescription>{};
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

// An endpoint advertising its own ApplicationDescription, as a configured
// server does: the discovery URLs are the listener's own URLs.
EndpointDescription WithDiscoveryUrls(EndpointDescription endpoint,
                                      std::vector<std::string> urls) {
  endpoint.server.application_uri = "urn:self";
  endpoint.server.discovery_urls = std::move(urls);
  return endpoint;
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

// The reported defect, second half: the bind host is a perfectly ordinary
// hostname that happens to resolve only inside the deployment network (a
// docker-compose service name). Nothing about "proxy" says unreachable, so the
// wildcard repair above cannot see it — but an outside client that follows this
// discovery URL is sent to an address it cannot resolve. The URL it dialled is
// the one address known to work, so that is what it gets offered first.
TEST_F(ServerRuntimeEndpointsTest, PrefersTheDialledUrlAmongOwnDiscoveryUrls) {
  auto runtime = MakeRuntime({WithDiscoveryUrls(
      TcpEndpoint("opc.tcp://proxy:4840"), {"opc.tcp://proxy:4840"})});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://127.0.0.1:14891");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_THAT(endpoints[0].server.discovery_urls,
              testing::ElementsAre("opc.tcp://127.0.0.1:14891",
                                   "opc.tcp://proxy:4840"));
}

// FindServers is the service a reconnecting client asks first, and the one that
// stranded the demo: open62541 adopts a returned discoveryUrl when none of them
// matches the URL it dialled, then fails to connect to it.
TEST_F(ServerRuntimeEndpointsTest, PrefersTheDialledUrlInFindServers) {
  auto runtime = MakeRuntime({WithDiscoveryUrls(
      TcpEndpoint("opc.tcp://proxy:4840"), {"opc.tcp://proxy:4840"})});

  const auto servers = FindServers(runtime, "opc.tcp://127.0.0.1:14891");

  ASSERT_EQ(servers.size(), 1u);
  EXPECT_THAT(servers[0].discovery_urls,
              testing::ElementsAre("opc.tcp://127.0.0.1:14891",
                                   "opc.tcp://proxy:4840"));
}

// Already the first entry: offering it again would advertise the same address
// twice.
TEST_F(ServerRuntimeEndpointsTest, DoesNotDuplicateAnAlreadyAdvertisedUrl) {
  auto runtime = MakeRuntime({WithDiscoveryUrls(
      TcpEndpoint("opc.tcp://gateway:4840"), {"opc.tcp://gateway:4840"})});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://gateway:4840");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_THAT(endpoints[0].server.discovery_urls,
              testing::ElementsAre("opc.tcp://gateway:4840"));
}

// A discovery URL of another transport is not an alternative address for the
// one the client dialled — it is a different listener, on its own port and
// path, and `advertise_url` is how a deployment states its public name. The
// client's URL must not displace it.
TEST_F(ServerRuntimeEndpointsTest, LeavesDiscoveryUrlsOfAnotherSchemeAlone) {
  auto runtime =
      MakeRuntime({WithDiscoveryUrls(WssEndpoint("opc.wss://demo.example/ua"),
                                     {"opc.wss://demo.example/ua"})});

  const auto endpoints = GetEndpoints(runtime, "opc.tcp://127.0.0.1:14891");

  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_THAT(endpoints[0].server.discovery_urls,
              testing::ElementsAre("opc.wss://demo.example/ua"));
}

// A registrant's discovery URL names *another* server. This one knows the
// caller reached *it* at the dialled URL, which says nothing about how a peer
// is reached, so a routable host configured for the peer stands.
TEST_F(ServerRuntimeEndpointsTest, KeepsRoutableRegistrantDiscoveryUrls) {
  auto runtime = MakeRuntime(
      {TcpEndpoint("opc.tcp://proxy:4840")},
      {RegisteredServer{.server_uri = "urn:historian",
                        .discovery_urls = {"opc.tcp://historian:4842"}}});

  const auto servers = FindServers(runtime, "opc.tcp://127.0.0.1:14891");

  const auto historian = std::ranges::find(
      servers, "urn:historian", &ApplicationDescription::application_uri);
  ASSERT_NE(historian, servers.end());
  EXPECT_THAT(historian->discovery_urls,
              testing::ElementsAre("opc.tcp://historian:4842"));
}

// CreateSession returns the endpoint list a client keeps for the rest of the
// session (OPC UA Part 4 §5.6.2) — including the reconnect that follows a
// dropped transport. It is the same set GetEndpoints returns, mapped the same
// way against the endpointUrl in the CreateSession body.
TEST_F(ServerRuntimeEndpointsTest, CreateSessionReturnsReachableEndpoints) {
  auto runtime =
      MakeRuntime({WithDiscoveryUrls(TcpEndpoint("opc.tcp://proxy:4840"),
                                     {"opc.tcp://proxy:4840"}),
                   WithDiscoveryUrls(WssEndpoint("opc.wss://0.0.0.0:4843/ua"),
                                     {"opc.wss://0.0.0.0:4843/ua"})});

  const auto endpoints =
      CreateSessionEndpoints(runtime, "opc.tcp://127.0.0.1:14891");

  ASSERT_EQ(endpoints.size(), 2u);
  EXPECT_EQ(UrlFor(endpoints, kTcpProfile), "opc.tcp://127.0.0.1:14891");
  EXPECT_EQ(UrlFor(endpoints, kWssProfile), "opc.wss://127.0.0.1:4843/ua");
  EXPECT_THAT(endpoints[0].server.discovery_urls,
              testing::ElementsAre("opc.tcp://127.0.0.1:14891",
                                   "opc.tcp://proxy:4840"));
}

// The two paths must not diverge: whatever a client is told at discovery is
// what it is told again when the session is created.
TEST_F(ServerRuntimeEndpointsTest, CreateSessionAgreesWithGetEndpoints) {
  auto runtime = MakeRuntime({TcpEndpoint("opc.tcp://proxy:4840"),
                              WssEndpoint("opc.wss://demo.example/ua")});

  const auto discovered = GetEndpoints(runtime, "opc.tcp://127.0.0.1:14891");
  const auto session =
      CreateSessionEndpoints(runtime, "opc.tcp://127.0.0.1:14891");

  ASSERT_EQ(session.size(), discovered.size());
  for (size_t i = 0; i < session.size(); ++i)
    EXPECT_EQ(session[i].endpoint_url, discovered[i].endpoint_url);
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
