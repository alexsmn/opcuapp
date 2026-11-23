#include "opcua/transport/binary/reverse_connect.h"

#include "opcua/base/boost_log.h"
#include "opcua/client/endpoint_url.h"
#include "opcua/session/login_lockout.h"

#include <transport/transport.h>
#include <transport/transport_string.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace opcua::binary {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaReverseConnect")};

constexpr std::size_t kHeaderSize = 8;
// Header, then two Strings each an Int32 length and at most 4095 bytes.
constexpr std::size_t kMaxReverseHelloSize =
    kHeaderSize + 2 * (4 + kMaxReverseHelloStringSize);

template <class T>
transport::awaitable<T> Fail(transport::error_code error) {
  co_return T{error};
}

transport::awaitable<transport::error_code> Succeed() {
  co_return transport::OK;
}

// Sleeps on `timer` until it expires or is cancelled.
Awaitable<void> Wait(boost::asio::steady_timer& timer) {
  [[maybe_unused]] auto [ec] = co_await timer.async_wait(
      boost::asio::as_tuple(boost::asio::use_awaitable));
}

// Writes all of `bytes`; false on any failure.
Awaitable<bool> WriteAll(const transport::any_transport& socket,
                         std::span<const char> bytes) {
  while (!bytes.empty()) {
    if (!socket)
      co_return false;
    auto written = co_await socket.write(bytes);
    if (!written.ok() || *written == 0)
      co_return false;
    bytes = bytes.subspan(*written);
  }
  co_return true;
}

}  // namespace

std::string ReverseConnectKey(std::string_view host, int port) {
  std::string key;
  key.reserve(host.size() + 6);
  for (const char c : host)
    key.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  key += ':';
  key += std::to_string(port);
  return key;
}

std::optional<std::string> ReverseConnectKey(std::string_view endpoint_url) {
  const ParsedEndpointUrl parsed = ParseOpcTcpUrl(std::string{endpoint_url});
  if (!parsed.valid || parsed.host.empty())
    return std::nullopt;
  return ReverseConnectKey(parsed.host, parsed.port);
}

// ---- ReverseConnectPool ----------------------------------------------------

struct ReverseConnectPool::Ticket {
  Ticket(transport::executor executor, std::string key)
      : executor{executor}, key{std::move(key)}, timer{executor} {}

  transport::executor executor;
  const std::string key;
  // Expired at time_point::min() to wake the waiter: that both cancels a wait
  // in flight and makes a wait not started yet complete at once.
  boost::asio::steady_timer timer;
  std::weak_ptr<Core> core;

  // Guarded by Core::mutex.
  std::optional<transport::any_transport> socket;
  bool waiting = false;
  bool cancelled = false;
  bool shutdown = false;
};

struct ReverseConnectPool::Core {
  struct Parked {
    transport::any_transport socket;
    ReverseConnectClock::time_point parked_at;
  };

  explicit Core(ReverseConnectPoolOptions options)
      : options{std::move(options)} {
    if (this->options.presence == ReverseConnectClock::duration{})
      this->options.presence = 2 * this->options.max_idle;
  }

  // Moves the expired sockets of `key` into `dropped`; caller holds `mutex`.
  void EvictExpired(const std::string& key,
                    std::vector<transport::any_transport>& dropped) {
    auto it = parked.find(key);
    if (it == parked.end())
      return;
    const auto now = options.now();
    auto& queue = it->second;
    while (!queue.empty() &&
           (now - queue.front().parked_at > options.max_idle ||
            !queue.front().socket)) {
      dropped.push_back(std::move(queue.front().socket));
      queue.pop_front();
    }
    if (queue.empty())
      parked.erase(it);
  }

  static void Wake(const std::shared_ptr<Ticket>& ticket) {
    boost::asio::post(ticket->executor, [ticket] {
      ticket->timer.expires_at(ReverseConnectClock::time_point::min());
    });
  }

  // Delivers `socket` to the oldest live waiter for `key`, or parks it.
  static void Park(const std::shared_ptr<Core>& core,
                   const std::string& key,
                   transport::any_transport socket) {
    std::vector<transport::any_transport> dropped;
    {
      std::lock_guard lock{core->mutex};
      if (core->closed) {
        dropped.push_back(std::move(socket));
      } else {
        core->last_parked[key] = core->options.now();
        auto& queue = core->waiters[key];
        while (!queue.empty()) {
          auto ticket = std::move(queue.front());
          queue.pop_front();
          if (!ticket->waiting || ticket->cancelled || ticket->socket)
            continue;
          ticket->socket = std::move(socket);
          Wake(ticket);
          break;
        }
        if (queue.empty())
          core->waiters.erase(key);
        if (socket) {
          auto& parked_queue = core->parked[key];
          parked_queue.push_back(
              Parked{std::move(socket), core->options.now()});
          core->EvictExpired(key, dropped);
          auto it = core->parked.find(key);
          while (it != core->parked.end() &&
                 it->second.size() > core->options.max_idle_per_endpoint) {
            dropped.push_back(std::move(it->second.front().socket));
            it->second.pop_front();
          }
        }
      }
    }
    // Destroyed outside the lock: destroying a transport closes its socket.
    dropped.clear();
  }

  static Awaitable<StatusOr<transport::any_transport>> Take(
      std::shared_ptr<Ticket> ticket,
      ReverseConnectClock::duration timeout) {
    using Result = StatusOr<transport::any_transport>;
    auto core = ticket->core.lock();
    if (!core)
      co_return Result{Status{StatusCode::Bad_Shutdown}};

    // Armed before the ticket is visible to Park(), so a wake that lands
    // before the wait below starts still ends it at once.
    ticket->timer.expires_after(timeout);
    std::vector<transport::any_transport> dropped;
    {
      std::lock_guard lock{core->mutex};
      if (core->closed)
        co_return Result{Status{StatusCode::Bad_Shutdown}};
      if (ticket->cancelled)
        co_return Result{Status{StatusCode::Bad_NoCommunication}};
      core->EvictExpired(ticket->key, dropped);
      if (auto it = core->parked.find(ticket->key); it != core->parked.end()) {
        // The newest: the one least likely to have been forgotten by a NAT.
        auto socket = std::move(it->second.back().socket);
        it->second.pop_back();
        if (it->second.empty())
          core->parked.erase(it);
        co_return Result{std::move(socket)};
      }
      ticket->waiting = true;
      core->waiters[ticket->key].push_back(ticket);
    }
    dropped.clear();

    co_await Wait(ticket->timer);

    std::optional<transport::any_transport> socket;
    Status status{StatusCode::Bad_Timeout};
    {
      std::lock_guard lock{core->mutex};
      ticket->waiting = false;
      if (auto it = core->waiters.find(ticket->key);
          it != core->waiters.end()) {
        std::erase(it->second, ticket);
        if (it->second.empty())
          core->waiters.erase(it);
      }
      socket = std::move(ticket->socket);
      ticket->socket.reset();
      if (ticket->shutdown)
        status = Status{StatusCode::Bad_Shutdown};
      else if (ticket->cancelled)
        status = Status{StatusCode::Bad_NoCommunication};
    }
    if (socket && *socket) {
      if (!ticket->cancelled && !ticket->shutdown)
        co_return Result{std::move(*socket)};
      // Its taker went away while it was on the way: it is still a good idle
      // socket, so give it to the next one.
      Park(core, ticket->key, std::move(*socket));
    }
    co_return Result{status};
  }

  ReverseConnectPoolOptions options;
  mutable std::mutex mutex;
  bool closed = false;
  std::map<std::string, std::deque<Parked>> parked;
  std::map<std::string, std::deque<std::shared_ptr<Ticket>>> waiters;
  std::map<std::string, ReverseConnectClock::time_point> last_parked;
  std::set<std::string> pinned;
};

ReverseConnectPool::ReverseConnectPool(ReverseConnectPoolOptions options)
    : core_{std::make_shared<Core>(std::move(options))} {}

ReverseConnectPool::~ReverseConnectPool() {
  std::vector<transport::any_transport> dropped;
  {
    std::lock_guard lock{core_->mutex};
    core_->closed = true;
    for (auto& [key, queue] : core_->parked) {
      for (auto& parked : queue)
        dropped.push_back(std::move(parked.socket));
    }
    core_->parked.clear();
    for (auto& [key, queue] : core_->waiters) {
      for (auto& ticket : queue) {
        ticket->shutdown = true;
        Core::Wake(ticket);
      }
    }
    core_->waiters.clear();
  }
}

void ReverseConnectPool::Park(const std::string& key,
                              transport::any_transport socket) {
  Core::Park(core_, key, std::move(socket));
}

std::shared_ptr<ReverseConnectPool::Ticket> ReverseConnectPool::MakeTicket(
    transport::executor executor,
    std::string key) {
  auto ticket = std::make_shared<Ticket>(executor, std::move(key));
  ticket->core = core_;
  return ticket;
}

// static
Awaitable<StatusOr<transport::any_transport>> ReverseConnectPool::Take(
    std::shared_ptr<Ticket> ticket,
    ReverseConnectClock::duration timeout) {
  return Core::Take(std::move(ticket), timeout);
}

// static
void ReverseConnectPool::Cancel(const std::shared_ptr<Ticket>& ticket) {
  auto core = ticket->core.lock();
  if (!core)
    return;
  std::lock_guard lock{core->mutex};
  ticket->cancelled = true;
  if (ticket->waiting)
    Core::Wake(ticket);
}

void ReverseConnectPool::Pin(const std::string& key) {
  std::lock_guard lock{core_->mutex};
  core_->pinned.insert(key);
}

void ReverseConnectPool::Unpin(const std::string& key) {
  std::lock_guard lock{core_->mutex};
  core_->pinned.erase(key);
}

bool ReverseConnectPool::ServesByReverse(const std::string& key) const {
  std::lock_guard lock{core_->mutex};
  if (core_->pinned.contains(key))
    return true;
  const auto it = core_->last_parked.find(key);
  return it != core_->last_parked.end() &&
         core_->options.now() - it->second <= core_->options.presence;
}

std::size_t ReverseConnectPool::idle_count(const std::string& key) const {
  std::vector<transport::any_transport> dropped;
  std::size_t count = 0;
  {
    std::lock_guard lock{core_->mutex};
    core_->EvictExpired(key, dropped);
    if (auto it = core_->parked.find(key); it != core_->parked.end())
      count = it->second.size();
  }
  return count;
}

// ---- ReverseConnectTransportFactory -----------------------------------------

namespace {

// The transport a ClientSession gets for a reverse-connected endpoint: open()
// takes a parked socket; everything after is that socket's. Its state is
// shared with the waiting open(), so destroying this transport -- the way a
// connect is cancelled in this tree -- leaves the waiter touching only that.
class ReverseTransport final : public transport::Transport {
 public:
  ReverseTransport(ReverseConnectPool& pool,
                   transport::executor executor,
                   std::string key,
                   ReverseConnectClock::duration timeout)
      : executor_{executor},
        key_{key},
        timeout_{timeout},
        state_{std::make_shared<State>()} {
    state_->ticket = pool.MakeTicket(executor, std::move(key));
  }

  ~ReverseTransport() override { ReverseConnectPool::Cancel(state_->ticket); }

  transport::awaitable<transport::error_code> open() override {
    if (state_->socket)
      return Succeed();
    return Open(state_, timeout_);
  }

  transport::awaitable<transport::error_code> close() override {
    ReverseConnectPool::Cancel(state_->ticket);
    if (!state_->socket)
      return Succeed();
    return state_->socket.close();
  }

  transport::awaitable<transport::expected<transport::any_transport>> accept()
      override {
    return Fail<transport::expected<transport::any_transport>>(
        transport::ERR_NOT_IMPLEMENTED);
  }

  transport::awaitable<transport::expected<size_t>> read(
      std::span<char> data) override {
    if (!state_->socket)
      return Fail<transport::expected<size_t>>(transport::ERR_INVALID_HANDLE);
    return state_->socket.read(data);
  }

  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) override {
    if (!state_->socket)
      return Fail<transport::expected<size_t>>(transport::ERR_INVALID_HANDLE);
    return state_->socket.write(data);
  }

  std::string name() const override { return "OPC UA reverse " + key_; }
  std::string peer() const override {
    return state_->socket ? state_->socket.peer() : std::string{};
  }
  bool message_oriented() const override { return false; }
  bool connected() const override {
    return state_->socket && state_->socket.connected();
  }
  bool active() const override { return true; }
  transport::executor get_executor() override { return executor_; }

 private:
  struct State {
    std::shared_ptr<ReverseConnectPool::Ticket> ticket;
    transport::any_transport socket;
  };

  static transport::awaitable<transport::error_code> Open(
      std::shared_ptr<State> state,
      ReverseConnectClock::duration timeout) {
    auto taken = co_await ReverseConnectPool::Take(state->ticket, timeout);
    if (!taken.ok()) {
      LOG_INFO(logger_) << "OPC UA reverse connect: no socket to take"
                        << LOG_TAG("Endpoint", state->ticket->key)
                        << LOG_TAG("Status", ToString(taken.status()));
      co_return taken.status().code() == StatusCode::Bad_Timeout
          ? transport::ERR_TIMED_OUT
          : transport::ERR_ABORTED;
    }
    state->socket = std::move(*taken);
    co_return transport::OK;
  }

  const transport::executor executor_;
  const std::string key_;
  const ReverseConnectClock::duration timeout_;
  const std::shared_ptr<State> state_;
};

}  // namespace

ReverseConnectTransportFactory::ReverseConnectTransportFactory(
    transport::TransportFactory& underlying,
    ReverseConnectPool& pool,
    ReverseConnectClock::duration wait_timeout)
    : underlying_{underlying}, pool_{pool}, wait_timeout_{wait_timeout} {}

transport::expected<transport::any_transport>
ReverseConnectTransportFactory::CreateTransport(
    const transport::TransportString& transport_string,
    const transport::executor& executor,
    const transport::log_source& log) {
  if (transport_string.GetProtocol() == transport::TransportString::TCP &&
      transport_string.active()) {
    const std::string key = ReverseConnectKey(
        transport_string.GetParamStr(transport::TransportString::kParamHost),
        transport_string.GetParamInt(transport::TransportString::kParamPort));
    if (pool_.ServesByReverse(key)) {
      return transport::any_transport{std::make_unique<ReverseTransport>(
          pool_, executor, key, wait_timeout_)};
    }
  }
  return underlying_.CreateTransport(transport_string, executor, log);
}

// ---- ReverseConnectListener -------------------------------------------------

struct ReverseConnectListener::Core
    : std::enable_shared_from_this<ReverseConnectListener::Core> {
  // One socket reading its ReverseHello. Shared with the deadline handler,
  // which cancels the read by destroying the socket.
  struct Handshake {
    transport::any_transport socket;
    std::string peer;
    bool timed_out = false;
  };

  explicit Core(ReverseConnectListenerContext&& context)
      : acceptor{std::move(context.acceptor)},
        pool{context.pool},
        recognize{std::move(context.recognize)},
        hello_timeout{context.hello_timeout},
        max_connections{context.max_connections},
        max_connections_per_peer{context.max_connections_per_peer} {}

  bool Admit(const std::string& address) {
    if (max_connections != 0 && pending >= max_connections)
      return false;
    if (max_connections_per_peer != 0) {
      const auto it = pending_per_peer.find(address);
      if (it != pending_per_peer.end() &&
          it->second >= max_connections_per_peer)
        return false;
    }
    return true;
  }

  static Awaitable<void> Refuse(transport::any_transport socket,
                                Status error,
                                std::string reason) {
    const auto frame =
        EncodeErrorMessage(ErrorMessage{.error = error, .reason = reason});
    (void)co_await WriteAll(socket, frame);
    if (socket)
      (void)co_await socket.close();
  }

  static Awaitable<bool> ReadExact(std::shared_ptr<Handshake> handshake,
                                   std::vector<char>& buffer,
                                   std::size_t size) {
    const std::size_t start = buffer.size();
    buffer.resize(start + size);
    std::size_t received = 0;
    while (received < size) {
      if (!handshake->socket)
        co_return false;
      auto read = co_await handshake->socket.read(
          std::span<char>{buffer.data() + start + received, size - received});
      if (!read.ok() || *read == 0)
        co_return false;
      received += *read;
    }
    co_return true;
  }

  static Awaitable<void> AcceptLoop(std::shared_ptr<Core> core) {
    while (core->opened) {
      auto accepted = co_await core->acceptor.accept();
      if (!accepted.ok() || !core->opened)
        co_return;
      auto socket = std::move(accepted.value());
      const std::string peer = socket.peer();
      const std::string address = PeerAddress(peer);
      const auto executor = socket.get_executor();
      if (!core->Admit(address)) {
        LOG_WARNING(logger_)
            << "OPC UA reverse connection refused"
            << LOG_TAG("Reason", "TooManyConnections") << LOG_TAG("Peer", peer);
        CoSpawn(executor,
                [socket = std::move(socket)]() mutable -> Awaitable<void> {
                  co_await Refuse(std::move(socket),
                                  Status{StatusCode::Bad_ServerTooBusy},
                                  "Too many pending reverse connections");
                });
        continue;
      }
      ++core->pending;
      ++core->pending_per_peer[address];
      auto handshake = std::make_shared<Handshake>(
          Handshake{.socket = std::move(socket), .peer = peer});
      core->handshakes.push_back(handshake);
      CoSpawn(executor, [core, handshake, address]() -> Awaitable<void> {
        co_await HandleSocket(core, handshake);
        --core->pending;
        if (auto it = core->pending_per_peer.find(address);
            it != core->pending_per_peer.end() && --it->second == 0) {
          core->pending_per_peer.erase(it);
        }
        std::erase_if(core->handshakes,
                      [](const auto& weak) { return weak.expired(); });
      });
    }
  }

  static Awaitable<void> HandleSocket(std::shared_ptr<Core> core,
                                      std::shared_ptr<Handshake> handshake) {
    boost::asio::steady_timer deadline{handshake->socket.get_executor()};
    deadline.expires_after(core->hello_timeout);
    deadline.async_wait([handshake](boost::system::error_code ec) {
      if (ec == boost::asio::error::operation_aborted)
        return;
      handshake->timed_out = true;
      handshake->socket.reset();
    });

    std::vector<char> frame;
    bool ok = co_await ReadExact(handshake, frame, kHeaderSize);
    std::optional<FrameHeader> header;
    if (ok) {
      header = DecodeFrameHeader(frame);
      ok = header && header->message_type == MessageType::ReverseHello &&
           header->message_size <= kMaxReverseHelloSize;
      if (!ok) {
        deadline.cancel();
        LOG_WARNING(logger_) << "OPC UA reverse connection refused"
                             << LOG_TAG("Reason", "NotAReverseHello")
                             << LOG_TAG("Peer", handshake->peer);
        co_await Refuse(std::move(handshake->socket),
                        Status{StatusCode::Bad_TcpEndpointUrlInvalid},
                        "Expected a ReverseHello");
        co_return;
      }
      ok = co_await ReadExact(handshake, frame,
                              header->message_size - kHeaderSize);
    }
    deadline.cancel();
    if (!ok) {
      LOG_INFO(logger_) << "OPC UA reverse connection dropped"
                        << LOG_TAG("Reason", handshake->timed_out
                                                 ? "NoReverseHello"
                                                 : "Closed")
                        << LOG_TAG("Peer", handshake->peer);
      co_return;
    }
    if (!core->opened || !handshake->socket)
      co_return;

    const auto hello = DecodeReverseHelloMessage(frame);
    const auto key =
        hello ? ReverseConnectKey(hello->endpoint_url) : std::nullopt;
    if (!hello || !key || !core->recognize || !core->recognize(*hello, *key)) {
      LOG_WARNING(logger_) << "OPC UA reverse connection refused"
                           << LOG_TAG("Reason", "UnrecognizedServer")
                           << LOG_TAG("ServerUri",
                                      hello ? hello->server_uri : std::string{})
                           << LOG_TAG("EndpointUrl", hello ? hello->endpoint_url
                                                           : std::string{})
                           << LOG_TAG("Peer", handshake->peer);
      co_await Refuse(std::move(handshake->socket),
                      Status{StatusCode::Bad_TcpEndpointUrlInvalid},
                      "Unrecognized ServerUri or EndpointUrl");
      co_return;
    }
    LOG_INFO(logger_) << "OPC UA reverse connection parked"
                      << LOG_TAG("ServerUri", hello->server_uri)
                      << LOG_TAG("EndpointUrl", hello->endpoint_url)
                      << LOG_TAG("Peer", handshake->peer);
    core->pool.Park(*key, std::move(handshake->socket));
  }

  transport::any_transport acceptor;
  ReverseConnectPool& pool;
  ReverseHelloRecognizer recognize;
  const ReverseConnectClock::duration hello_timeout;
  const std::size_t max_connections;
  const std::size_t max_connections_per_peer;

  bool opened = false;
  std::size_t pending = 0;
  std::map<std::string, std::size_t> pending_per_peer;
  std::vector<std::weak_ptr<Handshake>> handshakes;
};

ReverseConnectListener::ReverseConnectListener(
    ReverseConnectListenerContext&& context)
    : core_{std::make_shared<Core>(std::move(context))} {}

ReverseConnectListener::~ReverseConnectListener() {
  // Destroying a transport is how an operation in flight is cancelled: the
  // accept and every ReverseHello read fail, and their coroutines -- which
  // hold `core_` -- see `opened` false and end without touching the pool or
  // the recognizer, both of which may be going away with the owner.
  core_->opened = false;
  core_->acceptor.reset();
  for (auto& weak : core_->handshakes) {
    if (auto handshake = weak.lock())
      handshake->socket.reset();
  }
}

Awaitable<transport::error_code> ReverseConnectListener::Open() {
  if (core_->opened)
    co_return transport::OK;
  auto error = co_await core_->acceptor.open();
  if (error)
    co_return error;
  core_->opened = true;
  CoSpawn(core_->acceptor.get_executor(), [core = core_]() -> Awaitable<void> {
    co_await Core::AcceptLoop(core);
  });
  co_return transport::OK;
}

Awaitable<void> ReverseConnectListener::Close() {
  if (!core_->opened)
    co_return;
  core_->opened = false;
  (void)co_await core_->acceptor.close();
  for (auto& weak : core_->handshakes) {
    if (auto handshake = weak.lock())
      handshake->socket.reset();
  }
}

// ---- ReverseConnector -------------------------------------------------------

struct ReverseConnector::Core
    : std::enable_shared_from_this<ReverseConnector::Core> {
  // A socket this connector dialled. Shared by the transport the Server reads
  // and by Stop(), which closes it by destroying it.
  struct Socket {
    transport::any_transport transport;
    std::string peer;
    // Set by the first bytes the Client sends (its Hello).
    bool engaged = false;
    // Set when the Client's first bytes are an Error Message.
    std::optional<ErrorMessage> refused;
    bool finished = false;
  };

  // What the Server's connection reads and writes: the dialled socket, with
  // its first read inspected to tell a taken socket from a refused one.
  class ObservedTransport final : public transport::Transport {
   public:
    ObservedTransport(std::shared_ptr<Socket> socket,
                      std::weak_ptr<Core> core,
                      transport::executor executor)
        : socket_{std::move(socket)},
          core_{std::move(core)},
          executor_{executor} {}

    transport::awaitable<transport::error_code> open() override {
      return Succeed();
    }
    transport::awaitable<transport::error_code> close() override {
      if (!socket_->transport)
        return Succeed();
      return socket_->transport.close();
    }
    transport::awaitable<transport::expected<transport::any_transport>> accept()
        override {
      return Fail<transport::expected<transport::any_transport>>(
          transport::ERR_NOT_IMPLEMENTED);
    }
    transport::awaitable<transport::expected<size_t>> read(
        std::span<char> data) override {
      if (!socket_->transport)
        return Fail<transport::expected<size_t>>(
            transport::ERR_CONNECTION_CLOSED);
      if (observed_)
        return socket_->transport.read(data);
      observed_ = true;
      return ObserveFirstRead(socket_->transport.read(data), data, socket_,
                              core_);
    }
    transport::awaitable<transport::expected<size_t>> write(
        std::span<const char> data) override {
      if (!socket_->transport)
        return Fail<transport::expected<size_t>>(
            transport::ERR_CONNECTION_CLOSED);
      return socket_->transport.write(data);
    }
    std::string name() const override { return "OPC UA reverse connection"; }
    std::string peer() const override { return socket_->peer; }
    bool message_oriented() const override { return false; }
    bool connected() const override {
      return socket_->transport && socket_->transport.connected();
    }
    bool active() const override { return true; }
    transport::executor get_executor() override { return executor_; }

   private:
    // Touches only its arguments after resuming, never `this`.
    static transport::awaitable<transport::expected<size_t>> ObserveFirstRead(
        transport::awaitable<transport::expected<size_t>> inner,
        std::span<char> data,
        std::shared_ptr<Socket> socket,
        std::weak_ptr<Core> weak_core) {
      auto result = co_await std::move(inner);
      if (!result.ok() || *result == 0)
        co_return result;
      if (*result >= 3 && std::memcmp(data.data(), "ERR", 3) == 0) {
        // §7.1.3 Table 78: the Client refused the socket. Swallowed and
        // reported as end of stream, so the connection closes quietly instead
        // of answering an Error with one.
        std::vector<char> frame{data.data(), data.data() + *result};
        socket->refused = DecodeErrorMessage(frame).value_or(
            ErrorMessage{.error = StatusCode::Bad, .reason = {}});
        if (auto core = weak_core.lock())
          core->Poke();
        co_return size_t{0};
      }
      socket->engaged = true;
      if (auto core = weak_core.lock()) {
        ++core->engaged;
        core->Poke();
      }
      co_return result;
    }

    const std::shared_ptr<Socket> socket_;
    const std::weak_ptr<Core> core_;
    const transport::executor executor_;
    bool observed_ = false;
  };

  explicit Core(ReverseConnectorContext&& context)
      : context{std::move(context)},
        wake{this->context.executor},
        drained{this->context.executor} {}

  void Poke() {
    wake.cancel();
    drained.cancel();
  }

  Awaitable<void> Sleep(ReverseConnectClock::duration duration) {
    wake.expires_after(duration);
    co_await Wait(wake);
  }

  Awaitable<void> SleepUntilPoked() {
    wake.expires_at(ReverseConnectClock::time_point::max());
    co_await Wait(wake);
  }

  void Stop() {
    stopped = true;
    if (dialing)
      dialing->transport.reset();
    for (auto& weak : sockets) {
      if (auto socket = weak.lock())
        socket->transport.reset();
    }
    Poke();
  }

  static Awaitable<void> Run(std::shared_ptr<Core> core) {
    const ParsedEndpointUrl client = ParseOpcTcpUrl(core->context.client_url);
    if (!client.valid) {
      LOG_ERROR(logger_) << "OPC UA reverse connect: invalid client URL"
                         << LOG_TAG("ClientUrl", core->context.client_url);
      co_return;
    }
    const auto hello_frame = EncodeReverseHelloMessage(core->context.hello);
    auto backoff = core->context.min_backoff;
    bool failing = false;

    while (!core->stopped) {
      if (core->engaged >= core->context.max_connections) {
        co_await core->SleepUntilPoked();
        continue;
      }

      transport::TransportString ts;
      ts.SetProtocol(transport::TransportString::TCP);
      ts.SetActive(true);
      ts.SetParam(transport::TransportString::kParamHost, client.host);
      ts.SetParam(transport::TransportString::kParamPort, client.port);
      auto created = core->context.transport_factory.CreateTransport(
          ts, core->context.executor, transport::log_source{});
      if (!created.ok()) {
        LOG_ERROR(logger_) << "OPC UA reverse connect: cannot create transport"
                           << LOG_TAG("ClientUrl", core->context.client_url);
        co_return;
      }

      auto socket = std::make_shared<Socket>();
      socket->transport = std::move(created.value());
      core->dialing = socket;
      const auto dialled_at = ReverseConnectClock::now();
      auto error = co_await socket->transport.open();
      bool sent = !error && socket->transport;
      if (sent) {
        socket->peer = socket->transport.peer();
        sent = co_await WriteAll(socket->transport, hello_frame);
      }
      core->dialing.reset();
      if (core->stopped)
        break;
      if (!sent) {
        if (!failing) {
          LOG_WARNING(logger_) << "OPC UA reverse connect: cannot reach client"
                               << LOG_TAG("ClientUrl", core->context.client_url)
                               << LOG_TAG("Error", error.message());
          failing = true;
        }
        co_await core->Sleep(backoff);
        backoff = std::min(2 * backoff, core->context.retry_delay);
        continue;
      }
      if (failing) {
        LOG_INFO(logger_) << "OPC UA reverse connect: client reachable again"
                          << LOG_TAG("ClientUrl", core->context.client_url);
        failing = false;
      }

      core->sockets.push_back(socket);
      ++core->serving;
      CoSpawn(core->context.executor, [core, socket]() -> Awaitable<void> {
        co_await core->context.serve(
            transport::any_transport{std::make_unique<ObservedTransport>(
                socket, core, core->context.executor)});
        socket->finished = true;
        socket->transport.reset();
        if (socket->engaged)
          --core->engaged;
        --core->serving;
        std::erase_if(core->sockets,
                      [](const auto& weak) { return weak.expired(); });
        core->Poke();
      });

      while (!socket->engaged && !socket->finished && !socket->refused &&
             !core->stopped) {
        co_await core->SleepUntilPoked();
      }
      if (core->stopped)
        break;
      if (socket->engaged) {
        backoff = core->context.min_backoff;
        continue;
      }
      if (socket->refused) {
        LOG_WARNING(logger_)
            << "OPC UA reverse connect: refused by client"
            << LOG_TAG("ClientUrl", core->context.client_url)
            << LOG_TAG("Status", ToString(socket->refused->error))
            << LOG_TAG("Reason", socket->refused->reason);
        co_await core->Sleep(core->context.retry_delay);
        continue;
      }
      // Closed without a word: evicted as idle if it lived, else refused.
      if (ReverseConnectClock::now() - dialled_at >=
          core->context.stable_lifetime) {
        backoff = core->context.min_backoff;
        continue;
      }
      co_await core->Sleep(backoff);
      backoff = std::min(2 * backoff, core->context.retry_delay);
    }
  }

  ReverseConnectorContext context;
  boost::asio::steady_timer wake;
  // Close() waits on this one, so a Poke() reaches it and the dial loop alike.
  boost::asio::steady_timer drained;
  // `serve` calls that have not returned.
  std::size_t serving = 0;
  bool started = false;
  bool stopped = false;
  std::size_t engaged = 0;
  std::shared_ptr<Socket> dialing;
  std::vector<std::weak_ptr<Socket>> sockets;
};

ReverseConnector::ReverseConnector(ReverseConnectorContext&& context)
    : core_{std::make_shared<Core>(std::move(context))} {}

ReverseConnector::~ReverseConnector() {
  if (!core_->stopped)
    core_->Stop();
}

void ReverseConnector::Start() {
  if (core_->started)
    return;
  core_->started = true;
  CoSpawn(core_->context.executor,
          [core = core_]() -> Awaitable<void> { co_await Core::Run(core); });
}

void ReverseConnector::Stop() {
  core_->Stop();
}

Awaitable<void> ReverseConnector::Close() {
  const auto core = core_;
  core->Stop();
  while (core->serving != 0) {
    core->drained.expires_at(ReverseConnectClock::time_point::max());
    co_await Wait(core->drained);
  }
}

std::size_t ReverseConnector::engaged_count() const {
  return core_->engaged;
}

}  // namespace opcua::binary
