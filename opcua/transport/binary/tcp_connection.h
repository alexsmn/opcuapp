#pragma once

#include "opcua/base/async_completion.h"
#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/egress_meter.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/transport/binary/secure_channel.h"

#include <transport/any_transport.h>
#include <transport/write_queue.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opcua::binary {

// SecureChannel binding handed to the service handler with each decoded
// service frame: whether the channel is secured and, if so, the client
// application instance certificate (DER) it presented.
struct SecureFrameContext {
  bool secure = false;
  ByteString client_certificate;
};

using SecureFrameHandler = std::function<Awaitable<
    std::optional<std::vector<char>>>(std::vector<char>, SecureFrameContext)>;

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
  // How long a peer may take from accept to its first service request over an
  // open SecureChannel (Hello, Acknowledge and OpenSecureChannel in between).
  // Past it the connection is closed. Zero waits indefinitely, which is what
  // every connection did before this existed.
  Duration handshake_timeout;
  // Daily byte budget shared by the listener's connections. When it is spent
  // the next response closes the connection instead of being written. Null
  // means no budget.
  std::shared_ptr<EgressBudget> egress_budget;
  // Pace service responses to this many bytes per second (one-second burst).
  // Zero does not pace.
  std::size_t send_bytes_per_second = 0;
  std::function<DateTime()> now = &DateTime::Now;
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

  // Accounts bytes about to be written against the egress budget.
  void RecordEgress(std::size_t bytes);
  // Arms handshake_timeout; see TcpConnectionContext.
  void ArmHandshakeTimeout();

  bool hello_received_ = false;
  // Set by the first service request on an open SecureChannel; the handshake
  // timeout does nothing once it is set.
  bool handshake_complete_ = false;
  ConnectionPacer pacer_{send_bytes_per_second, now};
  // Remote peer ("address:port") captured while the transport is connected,
  // so failure logs can still identify the client after disconnect.
  std::string peer_;
  SecureChannel secure_channel_;
  std::size_t pending_service_frames_ = 0;
  std::optional<base::AsyncCompletion> service_frames_drained_;

  // Liveness token for the detached service-frame coroutines StartServiceFrame
  // spawns. Run()'s drain covers every path that *returns*, but a cancelled
  // Run() — its frame destroyed at shutdown — cannot await anything, so a
  // frame resuming afterwards must be able to tell that this connection, and
  // the transport it owns, are gone before touching either. Held by value so
  // it expires exactly when this connection is destroyed.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // Reassembly state for a multi-chunk SecureMessage in progress.
  std::vector<char> partial_message_;
  std::optional<std::uint32_t> partial_request_id_;
  std::size_t partial_chunk_count_ = 0;
};

}  // namespace opcua::binary
