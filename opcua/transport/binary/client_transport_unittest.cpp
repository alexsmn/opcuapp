#include "opcua/transport/binary/client_transport.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/protocol.h"
#include "transport/transport.h"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opcua::binary {
namespace {

struct StreamPeerState {
  std::deque<std::string> incoming;
  std::vector<std::string> writes;
  bool opened = false;
  bool closed = false;
};

class ScriptedStreamTransport {
 public:
  ScriptedStreamTransport(transport::executor executor,
                          std::shared_ptr<StreamPeerState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedStreamTransport(ScriptedStreamTransport&&) = default;
  ScriptedStreamTransport& operator=(ScriptedStreamTransport&&) = default;
  ScriptedStreamTransport(const ScriptedStreamTransport&) = delete;
  ScriptedStreamTransport& operator=(const ScriptedStreamTransport&) = delete;

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }

  transport::awaitable<transport::expected<size_t>> read(std::span<char> data) {
    if (state_->incoming.empty()) {
      co_return size_t{0};
    }
    auto chunk = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    if (chunk.size() > data.size()) {
      co_return transport::ERR_INVALID_ARGUMENT;
    }
    std::ranges::copy(chunk, data.begin());
    co_return chunk.size();
  }

  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    state_->writes.emplace_back(data.begin(), data.end());
    co_return data.size();
  }

  std::string name() const { return "ScriptedStreamTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<StreamPeerState> state_;
};

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

using namespace std::chrono_literals;

// A peer that completes the handshake below the OPC UA layer and then says
// nothing: `read` never completes on its own. The destructor is what releases
// it, which is not a shortcut in the fake but `transport::any_transport`'s
// actual contract -- destroying the transport is how this codebase cancels an
// operation already in flight (see the ownership comment on
// any_transport::transport_), and a real socket behaves the same way, its
// pending handler running with operation_aborted.
class SilentStreamTransport {
 public:
  SilentStreamTransport(boost::asio::any_io_executor executor,
                        std::shared_ptr<StreamPeerState> state)
      : executor_{std::move(executor)},
        state_{std::move(state)},
        parked_{std::make_shared<boost::asio::steady_timer>(executor_)} {
    parked_->expires_at(boost::asio::steady_timer::time_point::max());
  }
  SilentStreamTransport(SilentStreamTransport&&) = default;
  SilentStreamTransport& operator=(SilentStreamTransport&&) = default;
  SilentStreamTransport(const SilentStreamTransport&) = delete;
  SilentStreamTransport& operator=(const SilentStreamTransport&) = delete;

  // A moved-from instance holds no timer, so only the live one releases the
  // parked read.
  ~SilentStreamTransport() {
    if (parked_) {
      parked_->cancel();
    }
  }

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }

  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }

  transport::awaitable<transport::expected<size_t>> read(std::span<char>) {
    auto parked = parked_;
    boost::system::error_code ec;
    co_await parked->async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    co_return transport::ERR_ABORTED;
  }

  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    state_->writes.emplace_back(data.begin(), data.end());
    co_return data.size();
  }

  std::string name() const { return "SilentStreamTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  boost::asio::any_io_executor executor_;
  std::shared_ptr<StreamPeerState> state_;
  std::shared_ptr<boost::asio::steady_timer> parked_;
};

class ClientTransportTest : public ::testing::Test {
 protected:
  std::unique_ptr<ClientTransport> MakeClient(
      const std::shared_ptr<StreamPeerState>& peer,
      std::string endpoint_url = "opc.tcp://localhost:4840",
      TransportLimits limits = {}) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport = transport::any_transport{ScriptedStreamTransport{
            any_executor_, peer}},
        .endpoint_url = std::move(endpoint_url),
        .limits = limits,
    });
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

// The connect deadline needs a real timer, so this fixture runs an io_context
// rather than the TestExecutor the tests above use -- the same reason
// ClientChannelTimeoutTest does.
class ClientTransportConnectTimeoutTest : public ::testing::Test {
 protected:
  static constexpr auto kConnectTimeout = 50ms;
  // Generous against the deadline so a slow machine cannot fail the test, and
  // still far below anything a hang would take.
  static constexpr auto kRunCap = 5s;

  std::unique_ptr<ClientTransport> MakeSilentClient(
      const std::shared_ptr<StreamPeerState>& peer,
      std::optional<std::chrono::steady_clock::duration> timeout) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport = transport::any_transport{SilentStreamTransport{
            context_.get_executor(), peer}},
        .endpoint_url = "opc.tcp://localhost:4840",
        .limits = {},
        .connect_timeout = timeout,
    });
  }

  // Runs the context until `done` or the cap elapses. Returns whether it
  // finished, so a lost deadline is a failed expectation rather than a suite
  // that never returns.
  bool RunUntilDone(const std::shared_ptr<bool>& done,
                    std::chrono::steady_clock::duration cap) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (!*done && std::chrono::steady_clock::now() < deadline) {
      context_.restart();
      context_.run_for(5ms);
    }
    return *done;
  }

  boost::asio::io_context context_;
};

// The regression test for backlog 547's silent half. A peer that accepts the
// connection and never sends an Acknowledge used to park Connect() for ever:
// nothing below bounds the read, and ClientChannel::Call's deadline is not in
// play because nothing has been sent through it yet. The caller saw no error,
// no status and no log line -- which is what a historian looked like when its
// collection source wedged and its later processes recorded no connect attempt
// at all.
TEST_F(ClientTransportConnectTimeoutTest, ConnectTimesOutWhenPeerNeverAcks) {
  auto peer = std::make_shared<StreamPeerState>();
  auto client = MakeSilentClient(peer, kConnectTimeout);

  auto done = std::make_shared<bool>(false);
  auto status = std::make_shared<Status>(StatusCode::Good);
  boost::asio::co_spawn(
      context_,
      [&client, done, status]() -> Awaitable<void> {
        *status = co_await client->Connect();
        *done = true;
      },
      boost::asio::detached);

  ASSERT_TRUE(RunUntilDone(done, kRunCap)) << "Connect() never returned";
  EXPECT_EQ(status->code(), StatusCode::Bad_Timeout);
  EXPECT_FALSE(client->is_open());
  // The Hello did go out: this is a peer that took the connection and then
  // said nothing, not one that was never reachable.
  EXPECT_EQ(peer->writes.size(), 1u);
}

// std::nullopt restores the pre-deadline behaviour, which the server-side and
// e2e fixtures rely on. This is a negative assertion over a fixed span and so
// passes whether or not the span was long enough; what makes it meaningful is
// the test above, which shows a bounded connect settles in well under the cap
// this one waits out.
TEST_F(ClientTransportConnectTimeoutTest, ConnectWithoutTimeoutWaitsForTheAck) {
  auto peer = std::make_shared<StreamPeerState>();
  auto client = MakeSilentClient(peer, std::nullopt);

  auto done = std::make_shared<bool>(false);
  boost::asio::co_spawn(
      context_,
      [&client, done]() -> Awaitable<void> {
        (void)co_await client->Connect();
        *done = true;
      },
      boost::asio::detached);

  EXPECT_FALSE(RunUntilDone(done, 20 * kConnectTimeout))
      << "Connect() returned though no deadline was asked for";
  EXPECT_FALSE(*done);

  // Release the parked read so the coroutine unwinds before the context and
  // the client go away.
  client.reset();
  RunUntilDone(done, kRunCap);
}

TEST_F(ClientTransportTest, SendsHelloAndCapturesAcknowledge) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto ack_frame =
      EncodeAcknowledgeMessage({.protocol_version = 0,
                                .receive_buffer_size = 8192,
                                .send_buffer_size = 4096,
                                .max_message_size = 16 * 1024 * 1024,
                                .max_chunk_count = 0});
  peer->incoming.push_back(AsString(ack_frame));

  auto client = MakeClient(peer, "opc.tcp://localhost:4840",
                           {.protocol_version = 0,
                            .receive_buffer_size = 16384,
                            .send_buffer_size = 2048,
                            .max_message_size = 0,
                            .max_chunk_count = 0});

  const auto status = opcua::WaitAwaitable(executor_, client->Connect());
  EXPECT_TRUE(status.good());
  EXPECT_TRUE(client->is_open());
  EXPECT_EQ(client->acknowledge().receive_buffer_size, 8192u);
  EXPECT_EQ(client->acknowledge().send_buffer_size, 4096u);

  // A single Hello frame was written; decode it and verify the endpoint URL
  // + requested buffer sizes survive serialization.
  ASSERT_EQ(peer->writes.size(), 1u);
  const auto hello = DecodeHelloMessage(
      std::vector<char>{peer->writes[0].begin(), peer->writes[0].end()});
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ(hello->endpoint_url, "opc.tcp://localhost:4840");
  EXPECT_EQ(hello->receive_buffer_size, 16384u);
  EXPECT_EQ(hello->send_buffer_size, 2048u);
}

TEST_F(ClientTransportTest, PropagatesServerErrorReply) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto error_frame =
      EncodeErrorMessage({.error = opcua::StatusCode::Bad_NoCommunication,
                          .reason = "bad endpoint"});
  peer->incoming.push_back(AsString(error_frame));

  auto client = MakeClient(peer);
  const auto status = opcua::WaitAwaitable(executor_, client->Connect());
  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(client->is_open());
}

TEST_F(ClientTransportTest, ReadFrameReassemblesAcrossChunkedReads) {
  auto peer = std::make_shared<StreamPeerState>();
  const auto ack_frame = EncodeAcknowledgeMessage({.protocol_version = 0,
                                                   .receive_buffer_size = 65535,
                                                   .send_buffer_size = 65535});
  // Simulate reads split across two transport reads, to verify the reassembly
  // buffer inside ReadFrame.
  const std::size_t half = ack_frame.size() / 2;
  peer->incoming.push_back(AsString(std::vector<char>{
      ack_frame.begin(),
      ack_frame.begin() + static_cast<std::ptrdiff_t>(half)}));
  peer->incoming.push_back(AsString(std::vector<char>{
      ack_frame.begin() + static_cast<std::ptrdiff_t>(half), ack_frame.end()}));

  auto client = MakeClient(peer);
  const auto status = opcua::WaitAwaitable(executor_, client->Connect());
  EXPECT_TRUE(status.good());
  EXPECT_TRUE(client->is_open());
}

TEST_F(ClientTransportTest, WriteFrameForwardsBytesToTransport) {
  auto peer = std::make_shared<StreamPeerState>();
  peer->incoming.push_back(AsString(EncodeAcknowledgeMessage({})));

  auto client = MakeClient(peer);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client->Connect()).good());

  const std::vector<char> frame{'M', 'S', 'G', 'F', 0, 0, 0, 8};
  const auto write_status =
      opcua::WaitAwaitable(executor_, client->WriteFrame(frame));
  EXPECT_TRUE(write_status.good());

  ASSERT_EQ(peer->writes.size(), 2u);  // Hello + frame
  EXPECT_EQ(peer->writes[1], std::string(frame.begin(), frame.end()));
}

TEST_F(ClientTransportTest, ReadFrameReportsConnectionClosed) {
  auto peer = std::make_shared<StreamPeerState>();
  peer->incoming.push_back(AsString(EncodeAcknowledgeMessage({})));

  auto client = MakeClient(peer);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client->Connect()).good());

  // Peer has no more frames. ReadFrame should report Bad_ConnectionClosed.
  const auto read_result = opcua::WaitAwaitable(executor_, client->ReadFrame());
  EXPECT_FALSE(read_result.ok());
  EXPECT_TRUE(read_result.status().bad());
}

// A transport whose open() fails returns an error_code; the client must
// translate that into a bad Status and stay unopened.
class FailingOpenTransport {
 public:
  FailingOpenTransport(transport::executor executor,
                       std::shared_ptr<StreamPeerState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  FailingOpenTransport(FailingOpenTransport&&) = default;
  FailingOpenTransport& operator=(FailingOpenTransport&&) = default;
  FailingOpenTransport(const FailingOpenTransport&) = delete;
  FailingOpenTransport& operator=(const FailingOpenTransport&) = delete;

  transport::awaitable<transport::error_code> open() {
    co_return transport::ERR_FAILED;
  }
  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }
  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> read(std::span<char>) {
    co_return size_t{0};
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char>) {
    co_return transport::ERR_FAILED;
  }

  std::string name() const { return "FailingOpenTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return false; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<StreamPeerState> state_;
};

TEST_F(ClientTransportTest, ConnectFailsWhenTransportOpenFails) {
  auto peer = std::make_shared<StreamPeerState>();
  auto client = std::make_unique<ClientTransport>(ClientTransportContext{
      .transport =
          transport::any_transport{FailingOpenTransport{any_executor_, peer}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
  });

  const auto status = opcua::WaitAwaitable(executor_, client->Connect());
  EXPECT_TRUE(status.bad());
  EXPECT_FALSE(client->is_open());
}

TEST_F(ClientTransportTest, ReadFrameRejectsOversizedFrame) {
  auto peer = std::make_shared<StreamPeerState>();
  // Forge a frame header with message_size just above our max_frame_size.
  // The ACK frame passes Connect; the oversized frame is what the next
  // ReadFrame sees.
  peer->incoming.push_back(AsString(EncodeAcknowledgeMessage({})));
  const std::uint32_t kMax = 2048;
  const std::uint32_t oversized = kMax + 32;
  std::vector<char> bad_frame(8);
  bad_frame[0] = 'M';
  bad_frame[1] = 'S';
  bad_frame[2] = 'G';
  bad_frame[3] = 'F';
  bad_frame[4] = static_cast<char>(oversized & 0xff);
  bad_frame[5] = static_cast<char>((oversized >> 8) & 0xff);
  bad_frame[6] = static_cast<char>((oversized >> 16) & 0xff);
  bad_frame[7] = static_cast<char>((oversized >> 24) & 0xff);
  peer->incoming.push_back(AsString(bad_frame));

  auto client = std::make_unique<ClientTransport>(ClientTransportContext{
      .transport = transport::any_transport{ScriptedStreamTransport{
          any_executor_, peer}},
      .endpoint_url = "opc.tcp://localhost:4840",
      .limits = {},
      .max_frame_size = kMax,
  });
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client->Connect()).good());

  const auto read_result = opcua::WaitAwaitable(executor_, client->ReadFrame());
  EXPECT_FALSE(read_result.ok());
  EXPECT_TRUE(read_result.status().bad());
}

TEST_F(ClientTransportTest, CloseClearsIsOpen) {
  auto peer = std::make_shared<StreamPeerState>();
  peer->incoming.push_back(AsString(EncodeAcknowledgeMessage({})));

  auto client = MakeClient(peer);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client->Connect()).good());
  EXPECT_TRUE(client->is_open());

  opcua::WaitAwaitable(executor_, client->Close());
  EXPECT_FALSE(client->is_open());
  EXPECT_TRUE(peer->closed);
}

TEST_F(ClientTransportTest, AcknowledgeReflectsServerLimits) {
  auto peer = std::make_shared<StreamPeerState>();
  peer->incoming.push_back(
      AsString(EncodeAcknowledgeMessage({.protocol_version = 0,
                                         .receive_buffer_size = 1024,
                                         .send_buffer_size = 2048,
                                         .max_message_size = 500000,
                                         .max_chunk_count = 7})));

  auto client = MakeClient(peer);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client->Connect()).good());

  EXPECT_EQ(client->acknowledge().receive_buffer_size, 1024u);
  EXPECT_EQ(client->acknowledge().send_buffer_size, 2048u);
  EXPECT_EQ(client->acknowledge().max_message_size, 500000u);
  EXPECT_EQ(client->acknowledge().max_chunk_count, 7u);
}

TEST_F(ClientTransportTest, ReadFrameReturnsWriteQueuedFrame) {
  // Confirm that ReadFrame returns a complete frame once enough bytes arrive,
  // exercising the second branch of the reassembly loop (bytes buffered >
  // header size but < full frame).
  auto peer = std::make_shared<StreamPeerState>();
  const auto ack = EncodeAcknowledgeMessage({});
  peer->incoming.push_back(AsString(ack));
  // A second ACK-shaped frame follows, split across three reads.
  const auto next_frame =
      EncodeAcknowledgeMessage({.receive_buffer_size = 4096});
  ASSERT_GE(next_frame.size(), 9u);
  peer->incoming.push_back(
      AsString(std::vector<char>{next_frame.begin(), next_frame.begin() + 4}));
  peer->incoming.push_back(AsString(
      std::vector<char>{next_frame.begin() + 4, next_frame.begin() + 8}));
  peer->incoming.push_back(
      AsString(std::vector<char>{next_frame.begin() + 8, next_frame.end()}));

  auto client = MakeClient(peer);
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, client->Connect()).good());

  const auto read_result = opcua::WaitAwaitable(executor_, client->ReadFrame());
  ASSERT_TRUE(read_result.ok());
  EXPECT_EQ(read_result.value(), next_frame);
}

}  // namespace
}  // namespace opcua::binary
