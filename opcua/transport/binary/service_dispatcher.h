#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/service_codec.h"
#include "opcua/transport/binary/runtime.h"
#include "opcua/server_session_manager.h"

namespace opcua::binary {

class ServiceDispatcher {
 public:
  struct Context {
    Runtime& runtime;
    ConnectionState& connection;
  };

  explicit ServiceDispatcher(Context context);

  [[nodiscard]] Awaitable<std::optional<std::vector<char>>> HandlePayload(
      std::vector<char> payload);

 private:
  Runtime& runtime_;
  ConnectionState& connection_;
};

}  // namespace opcua::binary
