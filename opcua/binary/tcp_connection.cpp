#include "opcua/binary/tcp_connection.h"

#include "opcua/base/boost_log.h"

#include <algorithm>
#include <exception>

namespace opcua::binary {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaBinaryTcpConnection")};

// UA Secure Conversation chunk types (the 4th byte of the message header).
// OPC UA Part 6 §6.7.2,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7.2
constexpr char kIntermediateChunk = 'C';
constexpr char kFinalChunk = 'F';
constexpr char kAbortChunk = 'A';

// Safety cap on the number of chunks per message when the peer negotiated no
// max_chunk_count (0 = unlimited on the wire).
constexpr std::size_t kDefaultMaxChunkCount = 8192;

std::vector<char> SubspanToVector(const std::vector<char>& bytes,
                                  std::size_t offset,
                                  std::size_t size) {
  return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}

}  // namespace

TcpConnection::TcpConnection(
    TcpConnectionContext&& context)
    : TcpConnectionContext{std::move(context)},
      secure_channel_{secure_channel_config} {}

Awaitable<void> TcpConnection::Run() {
  [[maybe_unused]] auto open_result = co_await transport.open();
  transport::WriteQueue write_queue{transport};
  std::vector<char> read_buffer(read_buffer_size);
  std::vector<char> pending_bytes;

  for (;;) {
    auto read_result = co_await transport.read(read_buffer);
    if (!read_result.ok() || *read_result == 0) {
      break;
    }

    pending_bytes.insert(pending_bytes.end(), read_buffer.begin(),
                         read_buffer.begin() +
                             static_cast<std::ptrdiff_t>(*read_result));
    if (!(co_await ProcessBufferedFrames(write_queue, pending_bytes))) {
      break;
    }
  }

  co_await WaitForServiceFrames();
  [[maybe_unused]] auto close_result = co_await transport.close();
}

Awaitable<bool> TcpConnection::ProcessBufferedFrames(
    transport::WriteQueue& write_queue,
    std::vector<char>& pending_bytes) {
  for (;;) {
    if (pending_bytes.size() < 8) {
      co_return true;
    }

    const auto header = DecodeFrameHeader(
        std::vector<char>{pending_bytes.begin(), pending_bytes.begin() + 8});
    if (!header.has_value()) {
      co_return co_await WriteErrorAndClose(
          write_queue, scada::StatusCode::Bad, "Invalid UA TCP frame header");
    }
    if (header->message_size > max_frame_size) {
      co_return co_await WriteErrorAndClose(
          write_queue, scada::StatusCode::Bad, "UA TCP frame too large");
    }
    if (pending_bytes.size() < header->message_size) {
      co_return true;
    }

    auto frame = SubspanToVector(pending_bytes, 0, header->message_size);
    pending_bytes.erase(
        pending_bytes.begin(),
        pending_bytes.begin() + static_cast<std::ptrdiff_t>(header->message_size));
    if (!(co_await ProcessFrame(write_queue, frame))) {
      co_return false;
    }
  }
}

Awaitable<bool> TcpConnection::ProcessFrame(
    transport::WriteQueue& write_queue,
    const std::vector<char>& frame) {
  const auto header = DecodeFrameHeader(frame);
  if (!header.has_value()) {
    co_return co_await WriteErrorAndClose(
        write_queue, scada::StatusCode::Bad, "Invalid UA TCP frame header");
  }

  switch (header->message_type) {
    case MessageType::Hello: {
      if (hello_received_) {
        co_return co_await WriteErrorAndClose(
            write_queue, scada::StatusCode::Bad,
            "Hello may only be sent once per connection");
      }

      const auto hello = DecodeHelloMessage(frame);
      if (!hello.has_value()) {
        co_return co_await WriteErrorAndClose(
            write_queue, scada::StatusCode::Bad, "Malformed Hello message");
      }

      const auto negotiated = NegotiateHello(*hello, limits);
      if (negotiated.error.has_value()) {
        const auto encoded = EncodeErrorMessage(*negotiated.error);
        [[maybe_unused]] auto write_result =
            co_await write_queue.Write({encoded.data(), encoded.size()});
        co_return false;
      }

      hello_received_ = true;
      const auto encoded =
          EncodeAcknowledgeMessage(*negotiated.acknowledge);
      [[maybe_unused]] auto write_result =
          co_await write_queue.Write({encoded.data(), encoded.size()});
      co_return true;
    }

    case MessageType::SecureOpen:
    case MessageType::SecureMessage:
    case MessageType::SecureClose: {
      if (!hello_received_) {
        co_return co_await WriteErrorAndClose(
            write_queue, scada::StatusCode::Bad,
            "SecureChannel traffic received before Hello/Acknowledge");
      }

      auto result = co_await secure_channel_.HandleFrame(frame);
      if (result.outbound_frame.has_value() && !result.outbound_frame->empty()) {
        [[maybe_unused]] auto write_result =
            co_await write_queue.Write(
                {result.outbound_frame->data(), result.outbound_frame->size()});
      }
      if (result.close_transport) {
        co_return false;
      }
      if (result.service_payload.has_value() && result.request_id.has_value()) {
        co_return co_await ProcessSecureMessageChunk(
            write_queue, header->chunk_type,
            std::move(*result.service_payload), *result.request_id);
      }
      co_return true;
    }

    case MessageType::Acknowledge:
    case MessageType::Error:
    case MessageType::ReverseHello:
      co_return co_await WriteErrorAndClose(
          write_queue, scada::StatusCode::Bad,
          "Unexpected UA TCP message type for server connection");
  }

  co_return false;
}

Awaitable<bool> TcpConnection::ProcessSecureMessageChunk(
    transport::WriteQueue& write_queue,
    char chunk_type,
    std::vector<char> payload,
    std::uint32_t request_id) {
  // An abort chunk discards the partial message (OPC UA Part 6 §6.7.3,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7.3).
  if (chunk_type == kAbortChunk) {
    ResetReassembly();
    co_return true;
  }
  if (chunk_type != kIntermediateChunk && chunk_type != kFinalChunk) {
    ResetReassembly();
    co_return co_await WriteErrorAndClose(
        write_queue, scada::StatusCode::Bad,
        "Invalid UA Secure Conversation chunk type");
  }

  const std::size_t max_chunk_count =
      limits.max_chunk_count != 0 ? limits.max_chunk_count
                                  : kDefaultMaxChunkCount;
  if (partial_chunk_count_ + 1 > max_chunk_count) {
    ResetReassembly();
    co_return co_await WriteErrorAndClose(
        write_queue, scada::StatusCode::Bad,
        "UA Secure Conversation message exceeds max chunk count");
  }

  const std::size_t max_message_bytes =
      limits.max_message_size != 0 ? limits.max_message_size : max_frame_size;
  if (partial_message_.size() + payload.size() > max_message_bytes) {
    ResetReassembly();
    co_return co_await WriteErrorAndClose(
        write_queue, scada::StatusCode::Bad,
        "UA Secure Conversation message too large");
  }

  ++partial_chunk_count_;
  partial_request_id_ = request_id;
  partial_message_.insert(partial_message_.end(), payload.begin(),
                          payload.end());

  if (chunk_type == kIntermediateChunk) {
    co_return true;  // Wait for more chunks.
  }

  auto message = std::move(partial_message_);
  const auto reassembled_request_id = *partial_request_id_;
  ResetReassembly();
  StartServiceFrame(write_queue, std::move(message), reassembled_request_id);
  co_return true;
}

void TcpConnection::ResetReassembly() {
  partial_message_.clear();
  partial_request_id_.reset();
  partial_chunk_count_ = 0;
}

void TcpConnection::StartServiceFrame(transport::WriteQueue write_queue,
                                      std::vector<char> payload,
                                      std::uint32_t request_id) {
  if (pending_service_frames_++ == 0) {
    service_frames_drained_.emplace(transport.get_executor());
  }

  SecureFrameContext secure_context{
      .secure = secure_channel_.secure(),
      .client_certificate = secure_channel_.client_certificate(),
  };
  CoSpawn(transport.get_executor(),
          [this, write_queue = std::move(write_queue),
           payload = std::move(payload), request_id,
           secure_context = std::move(secure_context)]() mutable
          -> Awaitable<void> {
    try {
      auto outbound_payload = co_await on_secure_frame(
          std::move(payload), std::move(secure_context));
      if (outbound_payload.has_value() && !outbound_payload->empty()) {
        auto outbound_frame = secure_channel_.BuildServiceResponse(
            request_id, std::move(*outbound_payload));
        [[maybe_unused]] auto write_result = co_await write_queue.Write(
            {outbound_frame.data(), outbound_frame.size()});
      }
    } catch (const std::exception& e) {
      LOG_WARNING(logger_) << "OPC UA service frame handling failed"
                           << LOG_TAG("RequestId", request_id)
                           << LOG_TAG("Error", e.what());
    } catch (...) {
      LOG_WARNING(logger_) << "OPC UA service frame handling failed"
                           << LOG_TAG("RequestId", request_id);
    }
    FinishServiceFrame();
  });
}

Awaitable<void> TcpConnection::WaitForServiceFrames() {
  if (pending_service_frames_ == 0 || !service_frames_drained_.has_value()) {
    co_return;
  }
  co_await service_frames_drained_->Wait();
}

void TcpConnection::FinishServiceFrame() {
  if (pending_service_frames_ == 0) {
    return;
  }

  --pending_service_frames_;
  if (pending_service_frames_ == 0 && service_frames_drained_.has_value()) {
    service_frames_drained_->Complete();
  }
}

Awaitable<bool> TcpConnection::WriteErrorAndClose(
    transport::WriteQueue& write_queue,
    scada::Status error,
    std::string reason) {
  const auto encoded = EncodeErrorMessage(
      {.error = error, .reason = std::move(reason)});
  [[maybe_unused]] auto write_result =
      co_await write_queue.Write({encoded.data(), encoded.size()});
  co_return false;
}

}  // namespace opcua::binary
