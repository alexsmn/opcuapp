#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <cstdint>

namespace opcua {

struct ClientResponseFrame {
  std::uint32_t request_id = 0;
  ResponseMessage message;
};

// Transport/protocol adapter used by the reusable OPC UA client layer.
// Binary TCP and future WebSocket clients provide concrete implementations
// that translate typed request/response messages to their wire format.
class ClientConnection {
 public:
  ClientConnection() = default;
  ClientConnection(const ClientConnection&) = delete;
  ClientConnection& operator=(const ClientConnection&) = delete;
  virtual ~ClientConnection() = default;

  [[nodiscard]] virtual Awaitable<scada::Status> Open() = 0;
  [[nodiscard]] virtual Awaitable<scada::Status> Close() = 0;

  [[nodiscard]] virtual std::uint32_t NextRequestId() = 0;
  [[nodiscard]] virtual Awaitable<scada::Status> SendRequest(
      std::uint32_t request_id,
      const RequestMessage& message,
      const scada::NodeId& authentication_token) = 0;
  [[nodiscard]] virtual Awaitable<scada::StatusOr<ClientResponseFrame>>
  ReadResponse() = 0;
};

}  // namespace opcua
