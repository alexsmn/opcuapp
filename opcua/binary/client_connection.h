#pragma once

#include "opcua/binary/client_secure_channel.h"
#include "opcua/binary/client_transport.h"
#include "opcua/client_connection.h"

namespace opcua::binary {

class ClientConnection final : public opcua::ClientConnection {
 public:
  struct Context {
    ClientTransport& transport;
    ClientSecureChannel& secure_channel;
  };

  explicit ClientConnection(Context context);

  [[nodiscard]] Awaitable<Status> Open() override;
  [[nodiscard]] Awaitable<Status> Close() override;

  [[nodiscard]] std::uint32_t NextRequestId() override;
  [[nodiscard]] Awaitable<Status> SendRequest(
      std::uint32_t request_id,
      const RequestMessage& message,
      const NodeId& authentication_token) override;
  [[nodiscard]] Awaitable<StatusOr<ClientResponseFrame>>
  ReadResponse() override;

 private:
  ClientTransport& transport_;
  ClientSecureChannel& secure_channel_;
};

}  // namespace opcua::binary
