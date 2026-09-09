#include "opcua/client/client_channel.h"

#include "opcua/base/async_completion.h"

#include <boost/asio/execution/bad_executor.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>

namespace opcua {
namespace {

using namespace std::chrono_literals;

// Short enough to keep the suite quick, long enough that the ordinary
// send/receive path settles well inside it (DeliveredResponseIsNotTimedOut
// below is what establishes that, and NoTimeoutWaitsIndefinitely relies on it).
constexpr auto kRequestTimeout = 50ms;
// Upper bound on how long a test will wait for a bounded operation. A broken
// deadline must FAIL the test, not wedge the suite, so every run is capped.
constexpr auto kRunCap = 5s;

// A downstream that accepts the request and then says nothing at all, without
// closing the socket: the "TCP-absent but believed present" shape from the
// 2026-08-26 proxy incident. A peer that is merely *down* is handled correctly
// already — it fails fast — so a fake that closes cleanly would not exercise
// the defect. Here ReadResponse never completes, which is what a half-open
// connection looks like from above: no bytes and no error, ever.
class SilentConnection final : public ClientConnection {
 public:
  explicit SilentConnection(AnyExecutor executor)
      : reader_parked_{std::move(executor)} {}

  CoStatus Open() override { co_return Status{StatusCode::Good}; }
  CoStatus Close() override { co_return Status{StatusCode::Good}; }

  std::uint32_t NextRequestId() override { return ++last_request_id_; }

  CoStatus SendRequest(std::uint32_t /*request_id*/,
                       const RequestMessage& /*message*/,
                       const NodeId& /*authentication_token*/) override {
    if (std::exchange(throw_on_next_send_, false)) {
      // What ClientTransport::WriteFrame did after a write deadline had torn
      // the transport down: build a timer on an empty executor and throw.
      throw boost::asio::execution::bad_executor{};
    }
    ++sent_count_;
    co_return Status{StatusCode::Good};
  }

  // Makes the next SendRequest throw instead of completing.
  void ThrowOnNextSend() { throw_on_next_send_ = true; }

  CoStatusOr<ClientResponseFrame> ReadResponse() override {
    co_await reader_parked_.Wait();
    co_return StatusOr<ClientResponseFrame>{Status{StatusCode::Bad}};
  }

  // Lets the parked read loop unwind at teardown so no coroutine is still
  // suspended when the io_context goes away.
  void ReleaseReader() { reader_parked_.TryComplete(); }

  int sent_count() const { return sent_count_; }

 private:
  base::AsyncCompletion reader_parked_;
  std::uint32_t last_request_id_ = 0;
  int sent_count_ = 0;
  bool throw_on_next_send_ = false;
};

class ClientChannelTimeoutTest : public testing::Test {
 protected:
  // The channel is a fixture member, not a local in each test, because the read
  // loop is spawned with a raw `this`. Releasing the parked reader after the
  // channel had been destroyed would resume that coroutine on a dangling
  // pointer; here the channel is still alive during TearDown and outlives the
  // connection, since members die in reverse declaration order.
  void TearDown() override {
    connection_.ReleaseReader();
    DrainUntilIdle();
  }

  // Runs queued work until the context has none left, bounded so a stuck
  // teardown fails visibly instead of hanging.
  void DrainUntilIdle() {
    const auto deadline = std::chrono::steady_clock::now() + kRunCap;
    for (;;) {
      context_.restart();
      if (context_.run_for(10ms) == 0) {
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        ADD_FAILURE() << "Teardown did not settle";
        return;
      }
    }
  }

  // Runs the io_context until `done` or the cap elapses. Returns whether the
  // operation finished, so a lost deadline shows up as a failed expectation
  // rather than as a suite that never returns.
  bool RunUntilDone(const std::shared_ptr<bool>& done,
                    std::chrono::steady_clock::duration cap) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (!*done && std::chrono::steady_clock::now() < deadline) {
      context_.restart();
      context_.run_for(10ms);
    }
    return *done;
  }

  boost::asio::io_context context_;
  AnyExecutor executor_{context_.get_executor()};
  SilentConnection connection_{executor_};
  ClientChannel channel_{ClientChannel::Context{
      .executor = executor_,
      .connection = connection_,
      .authentication_token = {},
      .request_timeout = kRequestTimeout,
  }};
};

// The regression test for the wedge: before Call() had a deadline this waited
// for ever, and with it the caller learns the peer is not answering.
TEST_F(ClientChannelTimeoutTest, CallTimesOutWhenPeerNeverAnswers) {
  channel_.MarkLoginComplete();

  auto done = std::make_shared<bool>(false);
  auto status = std::make_shared<Status>(StatusCode::Good);
  // CloseSession deliberately: this is the request the aggregating proxy's
  // recovery path sends over the very connection it has just given up on, so
  // an unbounded wait here is what turned a dropped downstream into a
  // permanent outage rather than a reconnect.
  CoSpawn(executor_, [this, done, status]() -> Awaitable<void> {
    auto result = co_await channel_.Call(channel_.NextRequestHandle(),
                                         RequestBody{CloseSessionRequest{}});
    *status = result.status();
    *done = true;
  });

  ASSERT_TRUE(RunUntilDone(done, kRunCap)) << "Call() never returned";
  EXPECT_EQ(status->code(), StatusCode::Bad_Timeout);
  EXPECT_EQ(connection_.sent_count(), 1);
}

// The regression test for the send turn, which is what the aggregating proxy's
// `Disconnect` step was actually stuck behind for twelve hours (backlog 647).
// Send takes the turn, and until 2026-09-08 gave it back only on the
// `co_return`s -- so a throw out of SendRequest (the transport threw
// `bad_executor` on the first write after a deadline had torn it down) left
// `send_in_progress_` set for ever, inside a detached coroutine that logged
// nothing. Every later Send then parked in WaitForSendTurn before any deadline
// was armed: here the second Call never returned, and `done` stayed false.
TEST_F(ClientChannelTimeoutTest, AThrowingSendReleasesTheSendTurn) {
  channel_.MarkLoginComplete();
  connection_.ThrowOnNextSend();

  // The first call throws out of Send. CoSpawn is detached, so the exception
  // is dropped on the floor exactly as it was in production.
  CoSpawn(executor_, [this]() -> Awaitable<void> {
    (void)co_await channel_.Call(channel_.NextRequestHandle(),
                                 RequestBody{ua::ReadRequest{}});
  });

  auto done = std::make_shared<bool>(false);
  auto status = std::make_shared<Status>(StatusCode::Good);
  CoSpawn(executor_, [this, done, status]() -> Awaitable<void> {
    auto result = co_await channel_.Call(channel_.NextRequestHandle(),
                                         RequestBody{CloseSessionRequest{}});
    *status = result.status();
    *done = true;
  });

  ASSERT_TRUE(RunUntilDone(done, kRunCap))
      << "the send turn was never released; Call() parked in WaitForSendTurn";
  // The second call went out and waited on a peer that never answers, so its
  // own deadline is what ends it -- proof it got past the turn.
  EXPECT_EQ(status->code(), StatusCode::Bad_Timeout);
  EXPECT_EQ(connection_.sent_count(), 1);
}

// Publish uses the split Send/Receive API precisely because the server is
// entitled to hold the request until data is available (OPC UA Part 4 §5.14.5
// Publish, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5).
// A deadline on that path would tear down healthy subscriptions, so Receive
// must stay unbounded unless a caller asks otherwise.
//
// This is a negative assertion over a fixed span, which passes whether or not
// the span was long enough. What makes it meaningful is the test above: it
// shows a bounded request settles in well under kRequestTimeout, and this waits
// several times that.
TEST_F(ClientChannelTimeoutTest, ReceiveWithoutTimeoutWaitsIndefinitely) {
  channel_.MarkLoginComplete();

  auto done = std::make_shared<bool>(false);
  auto sent = std::make_shared<bool>(false);
  // A failed Send would leave nothing outstanding, so it would satisfy the
  // negative assertion below for the wrong reason; report it separately rather
  // than asserting inside the coroutine (a GoogleTest ASSERT_ expands to a bare
  // return, which a coroutine cannot use).
  CoSpawn(executor_, [this, done, sent]() -> Awaitable<void> {
    const std::uint32_t request_handle = channel_.NextRequestHandle();
    auto request_id =
        co_await channel_.Send(request_handle, RequestBody{PublishRequest{}});
    if (!request_id.ok()) {
      *done = true;
      co_return;
    }
    *sent = true;
    // No timeout argument: Publish's path.
    auto result = co_await channel_.Receive(*request_id, request_handle);
    (void)result;
    *done = true;
  });

  EXPECT_FALSE(RunUntilDone(done, kRequestTimeout * 10))
      << "Receive() without a deadline must not time out";
  EXPECT_TRUE(*sent) << "Send() did not complete, so nothing was outstanding";
}

}  // namespace
}  // namespace opcua
