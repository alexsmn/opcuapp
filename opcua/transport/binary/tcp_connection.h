#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/base/async_completion.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/transport/binary/secure_channel.h"

#include <transport/any_transport.h>
#include <transport/write_queue.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace opcua::binary {

// SecureChannel binding handed to the service handler with each decoded
// service frame: whether the channel is secured and, if so, the client
// application instance certificate (DER) it presented.
struct SecureFrameContext {
  bool secure = false;
  ByteString client_certificate;
};

using SecureFrameHandler =
    std::function<Awaitable<std::optional<std::vector<char>>>(
        std::vector<char>,
        SecureFrameContext)>;

struct TcpConnectionContext {
  transport::any_transport transport;
  TransportLimits limits;
  std::size_t read_buffer_size = 64 * 1024;
  std::size_t max_frame_size = 16 * 1024 * 1024;
  // Shared SecureChannel configuration. Null offers SecurityPolicy=None only.
  std::shared_ptr<const SecureChannelServerConfig> secure_channel_config;
  SecureFrameHandler on_secure_frame =
      [](std::vector<char>,
         SecureFrameContext) -> Awaitable<std::optional<std::vector<char>>> {
    co_return std::nullopt;
  };
};

class TcpConnection : private TcpConnectionContext {
 public:
  explicit TcpConnection(TcpConnectionContext&& context);

  [[nodiscard]] Awaitable<void> Run();

 private:
  [[nodiscard]] Awaitable<bool> ProcessBufferedFrames(
      transport::WriteQueue& write_queue,
      std::vector<char>& pending_bytes);
  [[nodiscard]] Awaitable<bool> ProcessFrame(transport::WriteQueue& write_queue,
                                            const std::vector<char>& frame);
  // Reassembles a SecureMessage split across MessageChunks: 'C' intermediate
  // chunk bodies are accumulated, 'F' final dispatches the whole message, 'A'
  // aborts and discards the partial message. Enforces max chunk count and total
  // message size. OPC UA Part 6 §6.7.2/§6.7.3,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7.2
  [[nodiscard]] Awaitable<bool> ProcessSecureMessageChunk(
      transport::WriteQueue& write_queue,
      char chunk_type,
      std::vector<char> payload,
      std::uint32_t request_id);
  void ResetReassembly();
  void StartServiceFrame(transport::WriteQueue write_queue,
                         std::vector<char> payload,
                         std::uint32_t request_id);
  [[nodiscard]] Awaitable<void> WaitForServiceFrames();
  void FinishServiceFrame();
  [[nodiscard]] Awaitable<bool> WriteErrorAndClose(
      transport::WriteQueue& write_queue,
      Status error,
      std::string reason);

  bool hello_received_ = false;
  SecureChannel secure_channel_;
  std::size_t pending_service_frames_ = 0;
  std::optional<base::AsyncCompletion> service_frames_drained_;

  // Reassembly state for a multi-chunk SecureMessage in progress.
  std::vector<char> partial_message_;
  std::optional<std::uint32_t> partial_request_id_;
  std::size_t partial_chunk_count_ = 0;
};

}  // namespace opcua::binary
