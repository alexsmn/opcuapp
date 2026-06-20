#include "opcua/binary/client_transport.h"

#include <algorithm>
#include <utility>

namespace opcua::binary {
namespace {

std::vector<char> SubspanToVector(const std::vector<char>& bytes,
                                  std::size_t offset,
                                  std::size_t size) {
  return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}

}  // namespace

ClientTransport::ClientTransport(
    ClientTransportContext&& context)
    : transport_{std::move(context.transport)},
      endpoint_url_{std::move(context.endpoint_url)},
      limits_{context.limits},
      read_buffer_size_{context.read_buffer_size},
      max_frame_size_{context.max_frame_size},
      write_queue_{transport_} {}

Awaitable<scada::Status> ClientTransport::Connect() {
  auto open_result = co_await transport_.open();
  if (open_result) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }

  const HelloMessage hello{
      .protocol_version = limits_.protocol_version,
      .receive_buffer_size = limits_.receive_buffer_size,
      .send_buffer_size = limits_.send_buffer_size,
      .max_message_size = limits_.max_message_size,
      .max_chunk_count = limits_.max_chunk_count,
      .endpoint_url = endpoint_url_,
  };
  const auto hello_bytes = EncodeHelloMessage(hello);
  auto write_result =
      co_await write_queue_.Write({hello_bytes.data(), hello_bytes.size()});
  if (!write_result.ok()) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }

  auto first_frame = co_await ReadFrame();
  if (!first_frame.ok()) {
    co_return first_frame.status();
  }

  const auto frame_header = DecodeFrameHeader(*first_frame);
  if (!frame_header.has_value()) {
    co_return scada::Status{scada::StatusCode::Bad};
  }

  switch (frame_header->message_type) {
    case MessageType::Acknowledge: {
      const auto ack = DecodeAcknowledgeMessage(*first_frame);
      if (!ack.has_value()) {
        co_return scada::Status{scada::StatusCode::Bad};
      }
      acknowledge_ = *ack;
      open_ = true;
      co_return scada::Status{scada::StatusCode::Good};
    }

    case MessageType::Error: {
      const auto error = DecodeErrorMessage(*first_frame);
      if (error.has_value()) {
        co_return error->error;
      }
      co_return scada::Status{scada::StatusCode::Bad_Disconnected};
    }

    default:
      co_return scada::Status{scada::StatusCode::Bad};
  }
}

Awaitable<scada::StatusOr<std::vector<char>>>
ClientTransport::ReadFrame() {
  std::vector<char> read_buffer(read_buffer_size_);
  for (;;) {
    if (pending_bytes_.size() >= 8) {
      const auto header = DecodeFrameHeader(std::vector<char>{
          pending_bytes_.begin(), pending_bytes_.begin() + 8});
      if (!header.has_value() || header->message_size < 8 ||
          header->message_size > max_frame_size_) {
        co_return scada::StatusOr<std::vector<char>>{
            scada::Status{scada::StatusCode::Bad}};
      }
      if (pending_bytes_.size() >= header->message_size) {
        auto frame = SubspanToVector(pending_bytes_, 0, header->message_size);
        pending_bytes_.erase(
            pending_bytes_.begin(),
            pending_bytes_.begin() +
                static_cast<std::ptrdiff_t>(header->message_size));
        co_return scada::StatusOr<std::vector<char>>{std::move(frame)};
      }
    }

    auto read_result = co_await transport_.read(read_buffer);
    if (!read_result.ok() || *read_result == 0) {
      co_return scada::StatusOr<std::vector<char>>{
          scada::Status{scada::StatusCode::Bad_Disconnected}};
    }
    pending_bytes_.insert(pending_bytes_.end(), read_buffer.begin(),
                          read_buffer.begin() +
                              static_cast<std::ptrdiff_t>(*read_result));
  }
}

Awaitable<scada::Status> ClientTransport::WriteFrame(
    const std::vector<char>& frame) {
  auto write_result =
      co_await write_queue_.Write({frame.data(), frame.size()});
  if (!write_result.ok()) {
    co_return scada::Status{scada::StatusCode::Bad_Disconnected};
  }
  co_return scada::Status{scada::StatusCode::Good};
}

Awaitable<void> ClientTransport::Close() {
  open_ = false;
  [[maybe_unused]] auto close_result = co_await transport_.close();
  co_return;
}

}  // namespace opcua::binary
