#include "opcua/binary/client_connection.h"

#include "opcua/binary/service_codec.h"

#include <utility>

namespace opcua::binary {

ClientConnection::ClientConnection(Context context)
    : transport_{context.transport},
      secure_channel_{context.secure_channel} {}

Awaitable<scada::Status> ClientConnection::Open() {
  auto connect_status = co_await transport_.Connect();
  if (connect_status.bad()) {
    co_return connect_status;
  }
  co_return co_await secure_channel_.Open();
}

Awaitable<scada::Status> ClientConnection::Close() {
  (void)(co_await secure_channel_.Close());
  co_await transport_.Close();
  co_return scada::Status{scada::StatusCode::Good};
}

std::uint32_t ClientConnection::NextRequestId() {
  return secure_channel_.NextRequestId();
}

Awaitable<scada::Status> ClientConnection::SendRequest(
    std::uint32_t request_id,
    const RequestMessage& message,
    const scada::NodeId& authentication_token) {
  const ServiceRequestHeader header{
      .authentication_token = authentication_token,
      .request_handle = message.request_handle,
  };
  const auto body = EncodeServiceRequest(header, message.body);
  if (!body.has_value()) {
    co_return scada::Status{scada::StatusCode::Bad};
  }
  co_return co_await secure_channel_.SendServiceRequest(request_id, *body);
}

Awaitable<scada::StatusOr<ClientResponseFrame>>
ClientConnection::ReadResponse() {
  auto response_frame = co_await secure_channel_.ReadServiceResponse();
  if (!response_frame.ok()) {
    co_return scada::StatusOr<ClientResponseFrame>{
        response_frame.status()};
  }

  auto decoded = DecodeServiceResponse(response_frame->body);
  if (!decoded.has_value()) {
    co_return scada::StatusOr<ClientResponseFrame>{
        scada::Status{scada::StatusCode::Bad}};
  }

  co_return scada::StatusOr<ClientResponseFrame>{
      ClientResponseFrame{
          .request_id = response_frame->request_id,
          .message = ResponseMessage{
              .request_handle = decoded->request_handle,
              .body = std::move(decoded->body),
          },
      }};
}

}  // namespace opcua::binary
