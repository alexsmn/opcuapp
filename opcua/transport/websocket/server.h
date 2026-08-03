#pragma once

#include "opcua/base/async_completion.h"
#include "opcua/session/server_runtime.h"

#include <transport/any_transport.h>
#include <transport/error.h>

#include <optional>

namespace opcua::ws {

struct ServerContext {
  transport::any_transport acceptor;
  ServerRuntime& runtime;
  size_t max_message_size = 4 * 1024 * 1024;
};

class Server : private ServerContext {
 public:
  explicit Server(ServerContext&& context);

  [[nodiscard]] Awaitable<transport::error_code> Open();
  [[nodiscard]] Awaitable<transport::error_code> Close();
  [[nodiscard]] Awaitable<void> ServeConnection(
      transport::any_transport transport);

 private:
  [[nodiscard]] Awaitable<void> AcceptLoop();
  [[nodiscard]] Awaitable<void> RunConnection(
      transport::any_transport transport);
  void TaskStarted();
  void TaskFinished();

  bool opened_ = false;
  bool closing_ = false;
  std::size_t active_tasks_ = 0;
  std::optional<base::AsyncCompletion> tasks_closed_;
};

}  // namespace opcua::ws
