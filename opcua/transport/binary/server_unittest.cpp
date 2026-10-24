#include "opcua/transport/binary/server.h"

#include "opcua/base/async_completion.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/session/authentication_adapters.h"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

// ConnectionLimits as binary::Server enforces them at accept, before a byte of
// the connection is read.

namespace opcua::binary {
namespace {

// One accepted connection: reports `peer`, records whether the server served
// it (open()), and holds its read open until the test releases it.
struct PeerState {
  explicit PeerState(transport::executor executor, std::string peer)
      : executor{executor},
        peer{std::move(peer)},
        release{std::move(executor)} {}
  transport::executor executor;
  std::string peer;
  bool opened = false;
  base::AsyncCompletion release;
};

class PeerTransport {
 public:
  explicit PeerTransport(std::shared_ptr<PeerState> state)
      : state_{std::move(state)} {}
  PeerTransport(PeerTransport&&) = default;
  PeerTransport& operator=(PeerTransport&&) = default;

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }
  transport::awaitable<transport::error_code> close() {
    if (!state_->release.completed())
      state_->release.Complete();
    co_return transport::OK;
  }
  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> read(std::span<char>) {
    auto state = state_;
    co_await state->release.Wait();
    co_return size_t{0};
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    co_return data.size();
  }
  std::string name() const { return "PeerTransport"; }
  std::string peer() const { return state_->peer; }
  bool message_oriented() const { return false; }
  bool connected() const { return true; }
  bool active() const { return true; }
  transport::executor get_executor() { return state_->executor; }

 private:
  std::shared_ptr<PeerState> state_;
};

// Hands out the queued connections, then parks until closed.
struct AcceptorState {
  explicit AcceptorState(transport::executor executor)
      : executor{executor}, closed{std::move(executor)} {}
  transport::executor executor;
  std::deque<std::shared_ptr<PeerState>> pending;
  base::AsyncCompletion closed;
};

class FakeAcceptor {
 public:
  explicit FakeAcceptor(std::shared_ptr<AcceptorState> state)
      : state_{std::move(state)} {}
  FakeAcceptor(FakeAcceptor&&) = default;
  FakeAcceptor& operator=(FakeAcceptor&&) = default;

  transport::awaitable<transport::error_code> open() {
    co_return transport::OK;
  }
  transport::awaitable<transport::error_code> close() {
    if (!state_->closed.completed())
      state_->closed.Complete();
    co_return transport::OK;
  }
  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    auto state = state_;
    if (state->pending.empty()) {
      co_await state->closed.Wait();
      co_return transport::ERR_CONNECTION_CLOSED;
    }
    auto next = std::move(state->pending.front());
    state->pending.pop_front();
    co_return transport::any_transport{PeerTransport{std::move(next)}};
  }
  transport::awaitable<transport::expected<size_t>> read(std::span<char>) {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char>) {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  std::string name() const { return "FakeAcceptor"; }
  bool message_oriented() const { return false; }
  bool connected() const { return true; }
  bool active() const { return false; }
  transport::executor get_executor() { return state_->executor; }

 private:
  std::shared_ptr<AcceptorState> state_;
};

class BinaryServerLimitsTest : public ::testing::Test {
 protected:
  std::shared_ptr<PeerState> Queue(std::string peer) {
    auto state = std::make_shared<PeerState>(any_executor_, std::move(peer));
    acceptor_->pending.push_back(state);
    return state;
  }

  // Opens a server over the queued connections, lets it accept them all, and
  // returns which were served. Then releases everything and closes.
  std::vector<bool> Serve(ConnectionLimits limits) {
    std::vector<std::shared_ptr<PeerState>> peers{acceptor_->pending.begin(),
                                                  acceptor_->pending.end()};
    Server server{
        {.acceptor = transport::any_transport{FakeAcceptor{acceptor_}},
         .runtime = runtime_,
         .session_manager = session_manager_,
         .connection_limits = limits}};
    EXPECT_EQ(WaitAwaitable(executor_, server.Open()), transport::OK);
    Drain(executor_);
    std::vector<bool> served;
    for (const auto& peer : peers)
      served.push_back(peer->opened);
    for (const auto& peer : peers) {
      if (!peer->release.completed())
        peer->release.Complete();
    }
    Drain(executor_);
    EXPECT_EQ(WaitAwaitable(executor_, server.Close()), transport::OK);
    return served;
  }

  TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  std::shared_ptr<AcceptorState> acceptor_ =
      std::make_shared<AcceptorState>(any_executor_);
  ServerSessionManager session_manager_{{
      .authenticator = MakeCoroutineAuthenticator(
          [](LocalizedText, LocalizedText) -> CoStatusOr<AuthenticationResult> {
            co_return AuthenticationResult{};
          }),
  }};
  Runtime runtime_{RuntimeContext{.executor = AnyExecutor{executor_},
                                  .session_manager = session_manager_}};
};

TEST_F(BinaryServerLimitsTest, ConnectionsPastTheTotalCapAreRefused) {
  Queue("10.0.0.1:1");
  Queue("10.0.0.2:1");
  Queue("10.0.0.3:1");
  EXPECT_EQ(Serve({.max_connections = 2}),
            (std::vector<bool>{true, true, false}));
}

TEST_F(BinaryServerLimitsTest, PerPeerCapCountsTheAddress) {
  Queue("10.0.0.1:1000");
  Queue("10.0.0.1:1001");
  Queue("10.0.0.1:1002");
  Queue("10.0.0.2:1000");
  EXPECT_EQ(Serve({.max_connections_per_peer = 2}),
            (std::vector<bool>{true, true, false, true}));
}

TEST_F(BinaryServerLimitsTest, DefaultsServeEveryone) {
  for (int i = 0; i < 10; ++i)
    Queue("10.0.0.1:" + std::to_string(1000 + i));
  EXPECT_EQ(Serve({}), std::vector<bool>(10, true));
}

}  // namespace
}  // namespace opcua::binary
