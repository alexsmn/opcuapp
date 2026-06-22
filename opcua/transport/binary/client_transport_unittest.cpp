#include "opcua/transport/binary/client_transport.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/protocol.h"
#include "transport/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
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
  const auto error_frame = EncodeErrorMessage(
      {.error = opcua::StatusCode::Bad_Disconnected, .reason = "bad endpoint"});
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
