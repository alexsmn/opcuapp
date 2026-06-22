#include "opcua/transport/binary/server.h"

#include "opcua/base/boost_log.h"
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

Server::Server(ServerContext&& context) : ServerContext{std::move(context)} {}

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
    auto executor = transport.get_executor();
    TaskStarted();
    CoSpawn(
        executor,
        [this, transport = std::move(transport)]() mutable -> Awaitable<void> {
          co_await ServeConnection(std::move(transport));
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
  auto state = std::make_shared<ConnectionTaskState>(std::move(transport));
  ServiceDispatcher dispatcher{
      {.runtime = *runtime_ptr, .connection = state->connection}};
  try {
    co_await TcpConnection{
        {.transport = std::move(state->transport),
         .read_buffer_size = read_buffer_size_value,
         .max_frame_size = max_frame_size_value,
         .secure_channel_config = std::move(secure_channel_config_value),
         .on_secure_frame = [&dispatcher, connection = &state->connection](
                                std::vector<char> payload,
                                SecureFrameContext secure_context)
             -> Awaitable<std::optional<std::vector<char>>> {
           connection->secure_channel = secure_context.secure;
           connection->client_certificate =
               std::move(secure_context.client_certificate);
           co_return co_await dispatcher.HandlePayload(std::move(payload));
         }}}
        .Run();
  } catch (const std::exception& e) {
    LOG_WARNING(logger_) << "OPC UA binary connection failed"
                         << LOG_TAG("Error", e.what());
  } catch (...) {
    LOG_WARNING(logger_) << "OPC UA binary connection failed";
  }
  state->connection.closed = true;
  runtime_ptr->Detach(state->connection);
}

}  // namespace opcua::binary
