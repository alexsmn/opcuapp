#pragma once

#include "opcua/base/async_completion.h"
#include "opcua/base/awaitable.h"
#include "opcua/transport/binary/egress_meter.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/transport/binary/runtime.h"
#include "opcua/transport/binary/secure_channel.h"

#include <transport/any_transport.h>
#include <transport/error.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace opcua::binary {

// Connection-level protections for a listener exposed to untrusted peers. Every
// field defaults to "no limit", which is what every listener had before these
// existed.
struct ConnectionLimits {
  // Connections held at once, across all peers and per peer address. A
  // connection over either is closed on accept, before its Hello is read.
  std::size_t max_connections = 0;
  std::size_t max_connections_per_peer = 0;
  // See TcpConnectionContext::handshake_timeout.
  Duration handshake_timeout;
  // Largest reassembled request, advertised in the Acknowledge
  // (MaxMessageSize, OPC UA Part 6 §7.1.2.4 Acknowledge Message,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/7.1.2.4) and
  // enforced on reassembly. Zero leaves the frame-size cap as the only bound.
  std::uint32_t max_message_size = 0;
  // Daily byte budget for the listener; zero is no budget. While it is spent,
  // new connections are refused on accept and live ones end at their next
  // response.
  std::uint64_t egress_bytes_per_day = 0;
  // Per-connection response pacing; zero does not pace.
  std::size_t send_bytes_per_second = 0;
};

struct ServerContext {
  transport::any_transport acceptor;
  Runtime& runtime;
  ServerSessionManager& session_manager;
  std::size_t read_buffer_size = 64 * 1024;
  std::size_t max_frame_size = 16 * 1024 * 1024;
  // Shared SecureChannel configuration. Null offers SecurityPolicy=None only.
  std::shared_ptr<const SecureChannelServerConfig> secure_channel_config;
  ConnectionLimits connection_limits;
  std::function<DateTime()> now = &DateTime::Now;
};

class Server : private ServerContext {
 public:
  explicit Server(ServerContext&& context);

  [[nodiscard]] Awaitable<transport::error_code> Open();
  [[nodiscard]] Awaitable<transport::error_code> Close();

  // Bytes this listener has sent since it opened and since 00:00 UTC; zero
  // when no egress budget is configured.
  [[nodiscard]] std::uint64_t egress_bytes_total() const;
  [[nodiscard]] std::uint64_t egress_bytes_today() const;
  [[nodiscard]] Awaitable<void> ServeConnection(
      transport::any_transport transport);

 private:
  [[nodiscard]] Awaitable<void> AcceptLoop();
  [[nodiscard]] Awaitable<void> RunConnection(
      transport::any_transport transport);
  void TaskStarted();
  void TaskFinished();

  // Whether an accepted connection from `peer` may be served, given the
  // ConnectionLimits; logs the refusal when it may not.
  [[nodiscard]] bool AdmitConnection(const std::string& peer_address);

  bool opened_ = false;
  std::size_t open_connections_ = 0;
  std::unordered_map<std::string, std::size_t> connections_per_peer_;
  std::shared_ptr<EgressBudget> egress_budget_;
  bool closing_ = false;
  std::size_t active_tasks_ = 0;
  std::optional<base::AsyncCompletion> tasks_closed_;
};

}  // namespace opcua::binary
