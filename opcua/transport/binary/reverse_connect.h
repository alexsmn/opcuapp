#pragma once

// OPC UA reverse connect: the Server opens the TransportConnection and sends a
// ReverseHello, and the Client answers on that socket with the ordinary Hello.
// OPC UA Part 6 §7.1.3 Establishing a connection,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/7.1.3, and §7.1.2.6
// ReverseHello Message,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/7.1.2.6.
//
// Client side: ReverseConnectListener accepts the sockets, reads and checks
// each ReverseHello and parks the socket in a ReverseConnectPool;
// ReverseConnectTransportFactory hands a parked socket to whatever asks the
// factory for that endpoint, so ClientSession and DiscoveryClient run unchanged
// over it. Server side: ReverseConnector keeps one idle socket open to a Client
// and hands each to a binary::Server.
//
// Design record: docs/server/opcua-reverse-connect.md (superproject).

#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <transport/any_transport.h>
#include <transport/executor.h>
#include <transport/transport_factory.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace opcua::binary {

using ReverseConnectClock = std::chrono::steady_clock;

// The key a ReverseHello's EndpointUrl and a ClientSession's dial target meet
// on: "host:port", host lower-cased. ClientSession hands the TransportFactory
// exactly the host and port of the endpoint URL, so these are the only parts
// both sides see. Nullopt for anything but a well-formed "opc.tcp://" URL.
[[nodiscard]] std::optional<std::string> ReverseConnectKey(
    std::string_view endpoint_url);
[[nodiscard]] std::string ReverseConnectKey(std::string_view host, int port);

struct ReverseConnectPoolOptions {
  // A parked socket older than this is closed rather than handed out: neither
  // end reads an idle socket, so a middlebox that forgot it is invisible until
  // a Hello goes into it. Closing it makes the Server dial a fresh one.
  ReverseConnectClock::duration max_idle = std::chrono::minutes{4};
  // Parked sockets kept per endpoint. Parking past it closes the OLDEST, so a
  // Server redialling after a dropped NAT mapping replaces its stale socket.
  std::size_t max_idle_per_endpoint = 2;
  // How long an endpoint that has had parked sockets stays served by reverse
  // connect after its last one (ServesByReverse). Zero means 2 × max_idle.
  ReverseConnectClock::duration presence = {};
  std::function<ReverseConnectClock::time_point()> now =
      &ReverseConnectClock::now;
};

// The sockets that arrived with a recognised ReverseHello, by endpoint key,
// waiting for a connect to take them. Thread-safe; a waiter is woken on its own
// executor.
class ReverseConnectPool {
 public:
  explicit ReverseConnectPool(ReverseConnectPoolOptions options = {});
  // Fails every waiting Take() with Bad_Shutdown and closes the parked sockets.
  ~ReverseConnectPool();

  ReverseConnectPool(const ReverseConnectPool&) = delete;
  ReverseConnectPool& operator=(const ReverseConnectPool&) = delete;

  // Hands `socket` to the oldest waiter for `key`, or parks it.
  void Park(const std::string& key, transport::any_transport socket);

  // The newest live parked socket for `key`, waiting up to `timeout` for one
  // to arrive: Bad_Timeout when none does, Bad_Shutdown when the pool goes
  // away. Destroying the caller's coroutine frame is NOT a way to cancel this
  // (an Awaitable cannot be abandoned mid-flight); ReverseTransport cancels
  // through Cancel() below.
  struct Ticket;
  [[nodiscard]] std::shared_ptr<Ticket> MakeTicket(transport::executor executor,
                                                   std::string key);
  [[nodiscard]] static Awaitable<StatusOr<transport::any_transport>> Take(
      std::shared_ptr<Ticket> ticket,
      ReverseConnectClock::duration timeout);
  // Ends a pending Take() with Bad_NoCommunication; a socket delivered to it
  // goes back to the pool.
  static void Cancel(const std::shared_ptr<Ticket>& ticket);

  // Endpoints a connect must take from the pool rather than dial. Pinned keys
  // always are (a static "reverse" downstream); any other key is while it has
  // had a parked socket within `presence`.
  void Pin(const std::string& key);
  void Unpin(const std::string& key);
  [[nodiscard]] bool ServesByReverse(const std::string& key) const;

  // Parked, unexpired sockets for `key`.
  [[nodiscard]] std::size_t idle_count(const std::string& key) const;

  struct Core;

 private:
  std::shared_ptr<Core> core_;
};

// A TransportFactory that serves the endpoints ReverseConnectPool says are
// reverse-connected from the pool, and dials everything else through
// `underlying`. A served transport's open() waits up to `wait_timeout` for a
// parked socket; everything after open() is the parked socket's.
class ReverseConnectTransportFactory final
    : public transport::TransportFactory {
 public:
  ReverseConnectTransportFactory(
      transport::TransportFactory& underlying,
      ReverseConnectPool& pool,
      ReverseConnectClock::duration wait_timeout = std::chrono::seconds{30});

  transport::expected<transport::any_transport> CreateTransport(
      const transport::TransportString& transport_string,
      const transport::executor& executor,
      const transport::log_source& log = {}) override;

 private:
  transport::TransportFactory& underlying_;
  ReverseConnectPool& pool_;
  const ReverseConnectClock::duration wait_timeout_;
};

// Decides whether a ReverseHello names a Server this Client serves by reverse
// connect (§7.1.2.6: "if it does not recognize the Server identified by the
// URI" / "the resource identified by the URL", refuse). `key` is
// ReverseConnectKey(hello.endpoint_url).
using ReverseHelloRecognizer =
    std::function<bool(const ReverseHelloMessage& hello,
                       const std::string& key)>;

struct ReverseConnectListenerContext {
  // A passive transport, opened by Open().
  transport::any_transport acceptor;
  ReverseConnectPool& pool;
  ReverseHelloRecognizer recognize;
  // §7.1.3 Table 78: the Client "shall close the socket after short delay if
  // no ReverseHello Message received".
  ReverseConnectClock::duration hello_timeout = std::chrono::seconds{10};
  // Sockets still reading their ReverseHello, in all and per peer address.
  // Past either a new socket gets ERR Bad_ServerTooBusy. Zero is no limit.
  std::size_t max_connections = 64;
  std::size_t max_connections_per_peer = 8;
};

// Accepts reverse connections and parks the recognised ones (see above).
class ReverseConnectListener {
 public:
  explicit ReverseConnectListener(ReverseConnectListenerContext&& context);
  ~ReverseConnectListener();

  ReverseConnectListener(const ReverseConnectListener&) = delete;
  ReverseConnectListener& operator=(const ReverseConnectListener&) = delete;

  [[nodiscard]] Awaitable<transport::error_code> Open();
  // Stops accepting. Parked sockets stay with the pool.
  [[nodiscard]] Awaitable<void> Close();

  struct Core;

 private:
  std::shared_ptr<Core> core_;
};

struct ReverseConnectorContext {
  transport::executor executor;
  transport::TransportFactory& transport_factory;
  // The Client's "opc.tcp://host:port".
  std::string client_url;
  // ServerUri = this Server's ApplicationUri; EndpointUrl = the URL its
  // endpoints are known by, which the Client sends back in its Hello.
  ReverseHelloMessage hello;
  // Serves one connection whose ReverseHello has been sent, to completion —
  // binary::Server::ServeConnection.
  std::function<Awaitable<void>(transport::any_transport)> serve;
  // Wait after the Client answers the ReverseHello with an Error (§7.1.3
  // Table 78: "reconnects after a delay specified in its configuration"), and
  // the ceiling of the backoff after failed dials.
  ReverseConnectClock::duration retry_delay = std::chrono::seconds{10};
  // First backoff step after a failed dial or a socket closed early.
  ReverseConnectClock::duration min_backoff = std::chrono::seconds{1};
  // A socket the Client closes without using after at least this long was
  // evicted as idle, not refused, and is replaced at once.
  ReverseConnectClock::duration stable_lifetime = std::chrono::seconds{30};
  // Connections the Client has taken and not yet closed; at the cap no new
  // idle socket is dialled until one ends.
  std::size_t max_connections = 8;
};

// Keeps one idle reverse socket open to one Client (§7.1.3 Table 78: "Servers
// shall maintain at least one open socket without an active Session with each
// Client it is configured to connect to"). A socket counts as taken from the
// first bytes the Client sends on it — its Hello — which is one step before
// the SecureChannel the table names, and keeps the same invariant.
class ReverseConnector {
 public:
  explicit ReverseConnector(ReverseConnectorContext&& context);
  // Stop() must have been called.
  ~ReverseConnector();

  ReverseConnector(const ReverseConnector&) = delete;
  ReverseConnector& operator=(const ReverseConnector&) = delete;

  void Start();
  // Ends the dial loop and closes every socket it dialled, the idle one and
  // the ones the Client took, which ends their `serve` calls.
  void Stop();
  // Stop(), then waits until every `serve` call has returned -- what an owner
  // whose `serve` reaches into a binary::Server must await before destroying
  // that server.
  [[nodiscard]] Awaitable<void> Close();

  // Connections the Client took and has not closed.
  [[nodiscard]] std::size_t engaged_count() const;

  struct Core;

 private:
  std::shared_ptr<Core> core_;
};

}  // namespace opcua::binary
