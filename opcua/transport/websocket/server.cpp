#include "opcua/transport/websocket/server.h"

#include "opcua/base/awaitable.h"
#include "opcua/base/boost_log.h"
#include "opcua/transport/websocket/json_codec.h"

#include <transport/write_queue.h>

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace opcua::ws {

namespace {

BoostLogger logger_{LOG_NAME("Server")};
std::atomic_uint64_t next_connection_id_{1};

template <typename T>
std::span<const char> AsCharSpan(const T& container) {
  return {reinterpret_cast<const char*>(container.data()), container.size()};
}

struct ConnectionTaskState {
  explicit ConnectionTaskState(transport::any_transport transport)
      : transport{std::move(transport)},
        write_queue{this->transport},
        connection_id{
            next_connection_id_.fetch_add(1, std::memory_order_relaxed)} {}

  transport::any_transport transport;
  transport::WriteQueue write_queue;
  ConnectionState connection;
  uint64_t connection_id;
};

}  // namespace

Server::Server(ServerContext&& context) : ServerContext{std::move(context)} {}

Awaitable<transport::error_code> Server::Open() {
  if (opened_)
    co_return transport::OK;

  auto error = co_await acceptor.open();
  if (error)
    co_return error;

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
  if (!opened_)
    co_return transport::OK;

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
    if (!accepted.ok())
      co_return;

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
  const auto max_message_size_value = max_message_size;
  auto state = std::make_shared<ConnectionTaskState>(std::move(transport));
  [[maybe_unused]] auto open_result = co_await state->transport.open();
  LOG_INFO(logger_) << "OPC UA WS connection opened"
                    << LOG_TAG("ConnectionId", state->connection_id)
                    << LOG_TAG("Transport", state->transport.name());
  std::vector<char> buffer(max_message_size_value);

  for (;;) {
    auto read_result = co_await state->transport.read(buffer);
    if (!read_result.ok() || *read_result == 0)
      break;

    StatusOr<RequestMessage> request{
        StatusCode::Bad_CantParseString};
    try {
      const std::string_view payload{buffer.data(), *read_result};
      request = DecodeRequestMessage(boost::json::parse(payload));
    } catch (const std::exception& e) {
      LOG_WARNING(logger_) << "OPC UA WS request parse failed"
                           << LOG_TAG("Reason", e.what());
    } catch (...) {
      LOG_WARNING(logger_) << "OPC UA WS request parse failed";
    }

    if (!request.ok()) {
      auto encoded = boost::json::serialize(EncodeJson(ResponseMessage{
          .request_handle = 0,
          .body =
              ServiceFault{.status = StatusCode::Bad_CantParseString}}));
      if (encoded.size() > max_message_size_value)
        break;

      auto write_result =
          co_await state->write_queue.Write(AsCharSpan(encoded));
      if (!write_result.ok())
        break;
      continue;
    }

    CoSpawn(state->transport.get_executor(),
            [runtime_ptr, max_message_size_value, state,
             request = std::move(*request)]() mutable -> Awaitable<void> {
              auto body = co_await runtime_ptr->Handle(state->connection,
                                                       std::move(request.body));
              auto response =
                  ResponseMessage{.request_handle = request.request_handle,
                                  .body = std::move(body)};
              auto encoded = boost::json::serialize(EncodeJson(response));
              if (encoded.size() > max_message_size_value)
                co_return;

              [[maybe_unused]] auto write_result =
                  co_await state->write_queue.Write(AsCharSpan(encoded));
            });
  }

  state->connection.closed = true;
  if (state->connection.authentication_token.has_value()) {
    LOG_INFO(logger_)
        << "OPC UA WS connection closed"
        << LOG_TAG("ConnectionId", state->connection_id)
        << LOG_TAG("Transport", state->transport.name())
        << LOG_TAG("AuthenticationToken",
                   state->connection.authentication_token->ToString());
  } else {
    LOG_INFO(logger_) << "OPC UA WS connection closed"
                      << LOG_TAG("ConnectionId", state->connection_id)
                      << LOG_TAG("Transport", state->transport.name());
  }
  runtime_ptr->Detach(state->connection);
  [[maybe_unused]] auto close_result = co_await state->transport.close();
}

}  // namespace opcua::ws
