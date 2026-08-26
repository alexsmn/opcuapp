#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/types/co_result.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <transport/any_transport.h>
#include <transport/write_queue.h>

#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opcua::binary {

// Deadline for a handshake read on this transport: the HEL/ACK exchange in
// Connect(), and the OpenSecureChannel response that ClientSecureChannel reads
// through ReadFrameWithin() below. A peer that completes the TCP handshake and
// then says nothing otherwise parks the caller for ever, and unlike a stalled
// request it is invisible: nothing has been sent through ClientChannel::Call
// yet, so its deadline is not in play, and a caller that only logs on a
// returned failure logs nothing at all. Superproject backlog 547 is that
// failure in the wild -- a historian whose collection source stopped and whose
// later processes recorded no connect attempt of any kind.
//
// Same 30 s as kDefaultClientRequestTimeout, deliberately a separate constant:
// the two bound different things -- a service call on a live channel and a
// handshake with a peer that may never have been alive -- and are free to
// diverge.
inline constexpr auto kDefaultHandshakeTimeout = std::chrono::seconds{30};

struct ClientTransportContext {
  transport::any_transport transport;
  std::string endpoint_url;
  TransportLimits limits;
  std::size_t read_buffer_size = 64 * 1024;
  std::size_t max_frame_size = 16 * 1024 * 1024;
  // Bounds every handshake read on this transport. std::nullopt waits
  // indefinitely, which is the pre-2026-08-26 behaviour.
  std::optional<std::chrono::steady_clock::duration> handshake_timeout =
      kDefaultHandshakeTimeout;
};

// Client-side analogue of TcpConnection. Owns a transport that the
// caller has already constructed (typically through transport::TransportFactory
// from an "opc.tcp://" URL), drives the OPC UA Part 6 HEL/ACK negotiation,
// and then exposes a raw frame read/write API for the secure channel layer to
// sit on top of. No SecureChannel logic lives here.
class ClientTransport {
 public:
  explicit ClientTransport(ClientTransportContext&& context);

  [[nodiscard]] CoStatus Connect();

  [[nodiscard]] CoStatusOr<std::vector<char>> ReadFrame();

  // ReadFrame() bounded by this transport's handshake_timeout, answering
  // Bad_Timeout if the peer does not. Used by the handshakes that run before
  // ClientChannel exists to bound them -- the ACK read in Connect() and the
  // OpenSecureChannel response in ClientSecureChannel. Cancelling the read
  // means destroying the transport, which is why this lives here rather than
  // in the callers: only the owner may do that, and it must outlive the read
  // it cancels.
  [[nodiscard]] CoStatusOr<std::vector<char>> ReadFrameWithin();
  [[nodiscard]] CoStatus WriteFrame(const std::vector<char>& frame);

  [[nodiscard]] Awaitable<void> Close();

  [[nodiscard]] const AcknowledgeMessage& acknowledge() const {
    return acknowledge_;
  }

  [[nodiscard]] bool is_open() const { return open_; }

 private:
  transport::any_transport transport_;
  const std::string endpoint_url_;
  const TransportLimits limits_;
  const std::size_t read_buffer_size_;
  const std::size_t max_frame_size_;
  const std::optional<std::chrono::steady_clock::duration> handshake_timeout_;
  transport::WriteQueue write_queue_;

  bool open_ = false;
  AcknowledgeMessage acknowledge_{};
  std::vector<char> pending_bytes_;
};

}  // namespace opcua::binary
