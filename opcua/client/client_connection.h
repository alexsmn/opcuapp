#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/message.h"
#include "opcua/types/co_result.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

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

  [[nodiscard]] virtual CoStatus Open() = 0;
  [[nodiscard]] virtual CoStatus Close() = 0;

  [[nodiscard]] virtual std::uint32_t NextRequestId() = 0;
  [[nodiscard]] virtual CoStatus SendRequest(
      std::uint32_t request_id,
      const RequestMessage& message,
      const NodeId& authentication_token) = 0;
  [[nodiscard]] virtual CoStatusOr<ClientResponseFrame> ReadResponse() = 0;

  // Security-token renewal hooks, driven by the request/response layer
  // (ClientChannel) rather than the transport's send path: the renewal
  // handshake reads its response directly off the transport, so it must never
  // run concurrently with the response read loop — the channel renews only
  // while it has no responses pending (single-reader invariant). Defaults are
  // for transports without a renewable token.
  [[nodiscard]] virtual bool ShouldRenewSecurityToken() const { return false; }
  [[nodiscard]] virtual CoStatus RenewSecurityToken() {
    co_return Status{StatusCode::Good};
  }
};

}  // namespace opcua
