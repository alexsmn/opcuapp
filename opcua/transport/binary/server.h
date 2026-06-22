#pragma once

#include "opcua/base/async_completion.h"
#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/runtime.h"
#include "opcua/transport/binary/secure_channel.h"

#include <transport/any_transport.h>
#include <transport/error.h>

#include <memory>
#include <optional>

namespace opcua::binary {

struct ServerContext {
  transport::any_transport acceptor;
  Runtime& runtime;
  ServerSessionManager& session_manager;
  std::size_t read_buffer_size = 64 * 1024;
  std::size_t max_frame_size = 16 * 1024 * 1024;
  // Shared SecureChannel configuration. Null offers SecurityPolicy=None only.
  std::shared_ptr<const SecureChannelServerConfig> secure_channel_config;
};

class Server : private ServerContext {
 public:
  explicit Server(ServerContext&& context);

  [[nodiscard]] Awaitable<transport::error_code> Open();
  [[nodiscard]] Awaitable<transport::error_code> Close();
  [[nodiscard]] Awaitable<void> ServeConnection(transport::any_transport transport);

 private:
  [[nodiscard]] Awaitable<void> AcceptLoop();
  [[nodiscard]] Awaitable<void> RunConnection(transport::any_transport transport);
  void TaskStarted();
  void TaskFinished();

  bool opened_ = false;
  bool closing_ = false;
  std::size_t active_tasks_ = 0;
  std::optional<base::AsyncCompletion> tasks_closed_;
};

}  // namespace opcua::binary
