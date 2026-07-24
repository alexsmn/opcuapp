#pragma once

#include "opcua/client/client_connection.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
#include "opcua/types/co_result.h"

namespace opcua::binary {

class ClientConnection final : public opcua::ClientConnection {
 public:
  struct Context {
    ClientTransport& transport;
    ClientSecureChannel& secure_channel;
  };

  explicit ClientConnection(Context context);

  [[nodiscard]] CoStatus Open() override;
  [[nodiscard]] CoStatus Close() override;

  [[nodiscard]] std::uint32_t NextRequestId() override;
  [[nodiscard]] CoStatus SendRequest(
      std::uint32_t request_id,
      const RequestMessage& message,
      const NodeId& authentication_token) override;
  [[nodiscard]] CoStatusOr<ClientResponseFrame> ReadResponse() override;
  [[nodiscard]] bool ShouldRenewSecurityToken() const override;
  [[nodiscard]] CoStatus RenewSecurityToken() override;

 private:
  ClientTransport& transport_;
  ClientSecureChannel& secure_channel_;
};

}  // namespace opcua::binary
