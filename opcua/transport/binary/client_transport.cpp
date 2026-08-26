#include "opcua/transport/binary/client_transport.h"
#include "opcua/types/co_result.h"

#include <boost/asio/error.hpp>

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

ClientTransport::ClientTransport(ClientTransportContext&& context)
    : transport_{std::move(context.transport)},
      endpoint_url_{std::move(context.endpoint_url)},
      limits_{context.limits},
      read_buffer_size_{context.read_buffer_size},
      max_frame_size_{context.max_frame_size},
      connect_timeout_{context.connect_timeout},
      write_queue_{transport_} {}

CoStatus ClientTransport::Connect() {
  auto open_result = co_await transport_.open();
  if (open_result) {
    co_return Status{StatusCode::Bad_NoCommunication};
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
    co_return Status{StatusCode::Bad_NoCommunication};
  }

  // Bound the ACK wait. The read below cannot be cancelled by asking it to
  // stop: `transport::any_transport`'s contract is that *destroying* the
  // transport is how this codebase cancels an operation in flight, and that a
  // pending read resumes touching only locals when it does (see the ownership
  // comment on any_transport::transport_). So the deadline handler resets the
  // transport, which fails the parked read and unwinds this coroutine
  // normally. Only the transport is destroyed -- this ClientTransport outlives
  // the handler and its own members stay valid, which is what makes the
  // documented mechanism safe to use here.
  auto timed_out = std::make_shared<bool>(false);
  boost::asio::steady_timer deadline{transport_.get_executor()};
  if (connect_timeout_) {
    deadline.expires_after(*connect_timeout_);
    deadline.async_wait([this, timed_out](boost::system::error_code ec) {
      if (ec == boost::asio::error::operation_aborted) {
        return;
      }
      *timed_out = true;
      transport_.reset();
    });
  }

  auto first_frame = co_await ReadFrame();
  deadline.cancel();
  if (*timed_out) {
    // Report the deadline rather than whatever error the torn-down read
    // produced: the read failing is this handler's own doing, and
    // Bad_NoCommunication would read as "the peer was unreachable" when in
    // fact it accepted the connection and then said nothing.
    co_return Status{StatusCode::Bad_Timeout};
  }
  if (!first_frame.ok()) {
    co_return first_frame.status();
  }

  const auto frame_header = DecodeFrameHeader(*first_frame);
  if (!frame_header.has_value()) {
    co_return Status{StatusCode::Bad};
  }

  switch (frame_header->message_type) {
    case MessageType::Acknowledge: {
      const auto ack = DecodeAcknowledgeMessage(*first_frame);
      if (!ack.has_value()) {
        co_return Status{StatusCode::Bad};
      }
      acknowledge_ = *ack;
      open_ = true;
      co_return Status{StatusCode::Good};
    }

    case MessageType::Error: {
      const auto error = DecodeErrorMessage(*first_frame);
      if (error.has_value()) {
        co_return error->error;
      }
      co_return Status{StatusCode::Bad_NoCommunication};
    }

    default:
      co_return Status{StatusCode::Bad};
  }
}

CoStatusOr<std::vector<char>> ClientTransport::ReadFrame() {
  std::vector<char> read_buffer(read_buffer_size_);
  for (;;) {
    if (pending_bytes_.size() >= 8) {
      const auto header = DecodeFrameHeader(std::vector<char>{
          pending_bytes_.begin(), pending_bytes_.begin() + 8});
      if (!header.has_value() || header->message_size < 8 ||
          header->message_size > max_frame_size_) {
        co_return StatusOr<std::vector<char>>{Status{StatusCode::Bad}};
      }
      if (pending_bytes_.size() >= header->message_size) {
        auto frame = SubspanToVector(pending_bytes_, 0, header->message_size);
        pending_bytes_.erase(
            pending_bytes_.begin(),
            pending_bytes_.begin() +
                static_cast<std::ptrdiff_t>(header->message_size));
        co_return StatusOr<std::vector<char>>{std::move(frame)};
      }
    }

    auto read_result = co_await transport_.read(read_buffer);
    if (!read_result.ok() || *read_result == 0) {
      co_return StatusOr<std::vector<char>>{
          Status{StatusCode::Bad_NoCommunication}};
    }
    pending_bytes_.insert(
        pending_bytes_.end(), read_buffer.begin(),
        read_buffer.begin() + static_cast<std::ptrdiff_t>(*read_result));
  }
}

CoStatus ClientTransport::WriteFrame(const std::vector<char>& frame) {
  auto write_result = co_await write_queue_.Write({frame.data(), frame.size()});
  if (!write_result.ok()) {
    co_return Status{StatusCode::Bad_NoCommunication};
  }
  co_return Status{StatusCode::Good};
}

Awaitable<void> ClientTransport::Close() {
  open_ = false;
  [[maybe_unused]] auto close_result = co_await transport_.close();
  co_return;
}

}  // namespace opcua::binary
