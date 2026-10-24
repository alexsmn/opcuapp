#include "opcua/transport/binary/server.h"

#include "opcua/base/boost_log.h"
#include "opcua/session/login_lockout.h"
#include "opcua/transport/binary/service_dispatcher.h"
#include "opcua/transport/binary/tcp_connection.h"

#include <exception>
#include <memory>

namespace opcua::binary {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaBinaryServer")};

struct ConnectionTaskState {
  explicit ConnectionTaskState(transport::any_transport transport)
      : transport{std::move(transport)} {}

  transport::any_transport transport;
  ConnectionState connection;
};

}  // namespace

Server::Server(ServerContext&& context)
    : ServerContext{std::move(context)},
      egress_budget_{connection_limits.egress_bytes_per_day != 0
                         ? std::make_shared<EgressBudget>(
                               connection_limits.egress_bytes_per_day,
                               now)
                         : nullptr} {}

std::uint64_t Server::egress_bytes_total() const {
  return egress_budget_ ? egress_budget_->sent_total() : 0;
}

std::uint64_t Server::egress_bytes_today() const {
  return egress_budget_ ? egress_budget_->sent_today() : 0;
}

bool Server::AdmitConnection(const std::string& peer_address) {
  const char* reason = nullptr;
  if (egress_budget_ && egress_budget_->exhausted()) {
    reason = "EgressBudgetSpent";
  } else if (connection_limits.max_connections != 0 &&
             open_connections_ >= connection_limits.max_connections) {
    reason = "TooManyConnections";
  } else if (connection_limits.max_connections_per_peer != 0) {
    const auto it = connections_per_peer_.find(peer_address);
    if (it != connections_per_peer_.end() &&
        it->second >= connection_limits.max_connections_per_peer) {
      reason = "TooManyConnectionsFromPeer";
    }
  }
  if (reason) {
    LOG_WARNING(logger_) << "OPC UA binary connection refused"
                         << LOG_TAG("Reason", reason)
                         << LOG_TAG("Peer", peer_address);
    return false;
  }
  return true;
}

Awaitable<transport::error_code> Server::Open() {
  if (opened_) {
    co_return transport::OK;
  }

  auto error = co_await acceptor.open();
  if (error) {
    co_return error;
  }

  opened_ = true;
  closing_ = false;
  active_tasks_ = 0;
  tasks_closed_.emplace(acceptor.get_executor());
  TaskStarted();
  CoSpawn(acceptor.get_executor(), [this]() -> Awaitable<void> {
    co_await AcceptLoop();
    TaskFinished();
  });
  co_return transport::OK;
}

Awaitable<transport::error_code> Server::Close() {
  if (!opened_) {
    co_return transport::OK;
  }

  opened_ = false;
  closing_ = true;
  auto error = co_await acceptor.close();
  if (active_tasks_ != 0 && tasks_closed_) {
    co_await tasks_closed_->Wait();
  }
  co_return error;
}

Awaitable<void> Server::ServeConnection(transport::any_transport transport) {
  co_await RunConnection(std::move(transport));
}

Awaitable<void> Server::AcceptLoop() {
  while (opened_) {
    auto accepted = co_await acceptor.accept();
    if (!accepted.ok()) {
      co_return;
    }

    auto transport = std::move(accepted.value());
    const auto peer_address = PeerAddress(transport.peer());
    if (!AdmitConnection(peer_address)) {
      // Destroying the transport closes the socket; nothing is read or sent.
      continue;
    }
    ++open_connections_;
    ++connections_per_peer_[peer_address];
    auto executor = transport.get_executor();
    TaskStarted();
    CoSpawn(executor,
            [this, peer_address,
             transport = std::move(transport)]() mutable -> Awaitable<void> {
              co_await ServeConnection(std::move(transport));
              --open_connections_;
              if (auto it = connections_per_peer_.find(peer_address);
                  it != connections_per_peer_.end() && --it->second == 0) {
                connections_per_peer_.erase(it);
              }
              TaskFinished();
            });
  }
}

void Server::TaskStarted() {
  ++active_tasks_;
}

void Server::TaskFinished() {
  assert(active_tasks_ != 0);
  --active_tasks_;
  if (closing_ && active_tasks_ == 0 && tasks_closed_ &&
      !tasks_closed_->completed()) {
    tasks_closed_->Complete();
  }
}

Awaitable<void> Server::RunConnection(transport::any_transport transport) {
  auto* runtime_ptr = &runtime;
  const auto read_buffer_size_value = read_buffer_size;
  const auto max_frame_size_value = max_frame_size;
  auto secure_channel_config_value = secure_channel_config;
  const auto limits_value = connection_limits;
  auto egress_budget_value = egress_budget_;
  auto now_value = now;
  auto state = std::make_shared<ConnectionTaskState>(std::move(transport));
  // Capture the remote peer while the socket is alive; it identifies the
  // client in connection, session, and per-request logs.
  state->connection.peer = state->transport.peer();
  LOG_INFO(logger_) << "OPC UA binary connection accepted"
                    << LOG_TAG("Peer", state->connection.peer);
  // Shared, and owned by the handler below together with `state`: a service
  // frame can still be suspended in HandlePayload when Run() unwinds and this
  // coroutine returns (a read failing mid-request), so the dispatcher and the
  // connection state it points at must live as long as the last frame, not as
  // long as this function. TcpConnection keeps a copy of the handler in each
  // frame for exactly that; the WebSocket server holds `state` the same way.
  auto dispatcher =
      std::make_shared<ServiceDispatcher>(ServiceDispatcher::Context{
          .runtime = *runtime_ptr, .connection = state->connection});
  try {
    co_await TcpConnection{
        {.transport = std::move(state->transport),
         .limits = {.max_message_size = limits_value.max_message_size},
         .read_buffer_size = read_buffer_size_value,
         .max_frame_size = max_frame_size_value,
         .secure_channel_config = std::move(secure_channel_config_value),
         .on_secure_frame = [dispatcher, state](
                                std::vector<char> payload,
                                SecureFrameContext secure_context)
             -> Awaitable<std::optional<std::vector<char>>> {
           state->connection.secure_channel = secure_context.secure;
           state->connection.client_certificate =
               std::move(secure_context.client_certificate);
           co_return co_await dispatcher->HandlePayload(std::move(payload));
         },
         .handshake_timeout = limits_value.handshake_timeout,
         .egress_budget = std::move(egress_budget_value),
         .send_bytes_per_second = limits_value.send_bytes_per_second,
         .now = std::move(now_value)}}
        .Run();
  } catch (const std::exception& e) {
    LOG_WARNING(logger_) << "OPC UA binary connection failed"
                         << LOG_TAG("Error", e.what())
                         << LOG_TAG("Peer", state->connection.peer);
  } catch (...) {
    LOG_WARNING(logger_) << "OPC UA binary connection failed"
                         << LOG_TAG("Peer", state->connection.peer);
  }
  LOG_INFO(logger_) << "OPC UA binary connection closed"
                    << LOG_TAG("Peer", state->connection.peer);
  state->connection.closed = true;
  runtime_ptr->Detach(state->connection);
}

}  // namespace opcua::binary
