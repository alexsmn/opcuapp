#include "opcua/transport/binary/reverse_connect.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/protocol.h"

#include <transport/tcp_transport.h>
#include <transport/transport.h>
#include <transport/transport_factory_impl.h>
#include <transport/transport_string.h>

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

// OPC UA reverse connect, OPC UA Part 6 §7.1.3 Establishing a connection,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/7.1.3. The pool is
// driven with fake sockets; the listener and the connector over loopback TCP.

namespace opcua::binary {
namespace {

using namespace std::chrono_literals;

constexpr char kServerUri[] = "urn:site:gateway";
constexpr char kEndpointUrl[] = "opc.tcp://GW.example:4844/path";
constexpr char kKey[] = "gw.example:4844";

// Drains `executor` until `done()` holds; false if it never does. Real sockets
// complete on the OS's time, so this polls rather than draining once.
bool RunUntil(TestExecutor& executor,
              const std::function<bool()>& done,
              std::chrono::milliseconds limit = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline)
      return false;
    executor.Poll();
    std::this_thread::yield();
  }
  return true;
}

// A socket that is only a name, for the pool.
class NamedSocket final : public transport::Transport {
 public:
  explicit NamedSocket(std::string name) : name_{std::move(name)} {}

  transport::awaitable<transport::error_code> open() override {
    co_return transport::OK;
  }
  transport::awaitable<transport::error_code> close() override {
    co_return transport::OK;
  }
  transport::awaitable<transport::expected<transport::any_transport>> accept()
      override {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> read(
      std::span<char>) override {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char>) override {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  std::string name() const override { return name_; }
  bool message_oriented() const override { return false; }
  bool connected() const override { return true; }
  bool active() const override { return false; }
  transport::executor get_executor() override { return {}; }

 private:
  const std::string name_;
};

transport::any_transport Named(std::string name) {
  return transport::any_transport{
      std::make_unique<NamedSocket>(std::move(name))};
}

TEST(ReverseConnectKeyTest, IsTheLowerCasedHostAndPort) {
  EXPECT_EQ(ReverseConnectKey(kEndpointUrl), kKey);
  EXPECT_EQ(ReverseConnectKey("opc.tcp://gw.example"), "gw.example:4840");
  EXPECT_EQ(ReverseConnectKey("GW.Example", 4844), kKey);
  EXPECT_FALSE(ReverseConnectKey("http://gw.example:4844").has_value());
  EXPECT_FALSE(ReverseConnectKey("").has_value());
}

class ReverseConnectPoolTest : public ::testing::Test {
 protected:
  ReverseConnectPoolOptions Options() {
    return {.max_idle = 4min, .max_idle_per_endpoint = 2, .now = [this] {
              return now_;
            }};
  }

  StatusOr<transport::any_transport> Take(
      ReverseConnectPool& pool,
      ReverseConnectClock::duration timeout = 1ms) {
    return WaitAwaitable(
        executor_,
        ReverseConnectPool::Take(pool.MakeTicket(executor_, kKey), timeout));
  }

  TestExecutor executor_;
  ReverseConnectClock::time_point now_ = ReverseConnectClock::now();
};

TEST_F(ReverseConnectPoolTest, HandsOutTheNewestParkedSocket) {
  ReverseConnectPool pool{Options()};
  pool.Park(kKey, Named("old"));
  pool.Park(kKey, Named("new"));
  auto taken = Take(pool);
  ASSERT_TRUE(taken.ok());
  EXPECT_EQ(taken->name(), "new");
  EXPECT_EQ(pool.idle_count(kKey), 1u);
}

TEST_F(ReverseConnectPoolTest, ParkingPastTheCapClosesTheOldest) {
  ReverseConnectPool pool{Options()};
  pool.Park(kKey, Named("a"));
  pool.Park(kKey, Named("b"));
  pool.Park(kKey, Named("c"));
  EXPECT_EQ(pool.idle_count(kKey), 2u);
  EXPECT_EQ(Take(pool)->name(), "c");
  EXPECT_EQ(Take(pool)->name(), "b");
  EXPECT_EQ(Take(pool).status().code(), StatusCode::Bad_Timeout);
}

TEST_F(ReverseConnectPoolTest, AnIdleSocketPastMaxIdleIsNotHandedOut) {
  ReverseConnectPool pool{Options()};
  pool.Park(kKey, Named("stale"));
  now_ += 4min + 1s;
  EXPECT_EQ(pool.idle_count(kKey), 0u);
  EXPECT_EQ(Take(pool).status().code(), StatusCode::Bad_Timeout);
}

TEST_F(ReverseConnectPoolTest, AWaitingTakeGetsTheNextParkedSocket) {
  ReverseConnectPool pool{Options()};
  auto result = StartAwaitable(
      executor_,
      ReverseConnectPool::Take(pool.MakeTicket(executor_, kKey), 10s));
  Drain(executor_);
  EXPECT_FALSE(result->done);

  pool.Park(kKey, Named("arrived"));
  ASSERT_TRUE(RunUntil(executor_, [&] { return result->done; }));
  ASSERT_TRUE(result->value->ok());
  EXPECT_EQ((*result->value)->name(), "arrived");
  EXPECT_EQ(pool.idle_count(kKey), 0u);
}

TEST_F(ReverseConnectPoolTest, ACancelledTakeEndsAndLeavesTheSocketParked) {
  ReverseConnectPool pool{Options()};
  auto ticket = pool.MakeTicket(executor_, kKey);
  auto result =
      StartAwaitable(executor_, ReverseConnectPool::Take(ticket, 10s));
  Drain(executor_);
  ReverseConnectPool::Cancel(ticket);
  ASSERT_TRUE(RunUntil(executor_, [&] { return result->done; }));
  EXPECT_EQ(result->value->status().code(), StatusCode::Bad_NoCommunication);

  pool.Park(kKey, Named("kept"));
  EXPECT_EQ(pool.idle_count(kKey), 1u);
}

TEST_F(ReverseConnectPoolTest, DestroyingThePoolFailsAWaitingTake) {
  auto pool = std::make_unique<ReverseConnectPool>(Options());
  auto result = StartAwaitable(
      executor_,
      ReverseConnectPool::Take(pool->MakeTicket(executor_, kKey), 10s));
  Drain(executor_);
  pool.reset();
  ASSERT_TRUE(RunUntil(executor_, [&] { return result->done; }));
  EXPECT_EQ(result->value->status().code(), StatusCode::Bad_Shutdown);
}

TEST_F(ReverseConnectPoolTest, ServesByReversePinnedOrRecentlyParkedKeys) {
  ReverseConnectPool pool{Options()};
  EXPECT_FALSE(pool.ServesByReverse(kKey));

  pool.Park(kKey, Named("s"));
  EXPECT_TRUE(pool.ServesByReverse(kKey));
  // Presence defaults to 2 × max_idle after the last park.
  now_ += 8min + 1s;
  EXPECT_FALSE(pool.ServesByReverse(kKey));

  pool.Pin(kKey);
  EXPECT_TRUE(pool.ServesByReverse(kKey));
  pool.Unpin(kKey);
  EXPECT_FALSE(pool.ServesByReverse(kKey));
}

// ---- Listener, connector and factory over loopback TCP ---------------------

class ReverseConnectTcpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto acceptor = std::make_unique<transport::PassiveTcpTransport>(
        any_executor_, transport::log_source{}, "127.0.0.1", "0");
    auto* acceptor_ptr = acceptor.get();
    listener_.emplace(ReverseConnectListenerContext{
        .acceptor = transport::any_transport{std::move(acceptor)},
        .pool = pool_,
        .recognize =
            [this](const ReverseHelloMessage& hello, const std::string& key) {
              recognized_.push_back(key);
              return hello.server_uri == kServerUri && key == kKey;
            },
        .hello_timeout = hello_timeout_,
    });
    ASSERT_EQ(WaitAwaitable(executor_, listener_->Open()), transport::OK);
    client_url_ =
        "opc.tcp://127.0.0.1:" + std::to_string(acceptor_ptr->GetLocalPort());
  }

  void TearDown() override {
    for (auto& connector : connectors_)
      connector->Stop();
    if (listener_)
      WaitAwaitable(executor_, listener_->Close());
    Drain(executor_);
  }

  // A raw TCP socket to the listener.
  transport::any_transport Dial() {
    transport::TransportString ts;
    ts.SetProtocol(transport::TransportString::TCP);
    ts.SetActive(true);
    ts.SetParam(transport::TransportString::kParamHost, "127.0.0.1");
    ts.SetParam(transport::TransportString::kParamPort,
                std::stoi(client_url_.substr(client_url_.rfind(':') + 1)));
    auto created = factory_.CreateTransport(ts, any_executor_);
    EXPECT_TRUE(created.ok());
    auto socket = std::move(created.value());
    EXPECT_EQ(WaitAwaitable(executor_, socket.open()), transport::OK);
    return socket;
  }

  void Send(transport::any_transport& socket, const std::vector<char>& bytes) {
    auto written = WaitAwaitable(executor_, socket.write(bytes));
    ASSERT_TRUE(written.ok());
    ASSERT_EQ(*written, bytes.size());
  }

  // Everything the peer sends until it closes.
  std::vector<char> ReadToEnd(transport::any_transport& socket) {
    std::vector<char> received;
    for (;;) {
      std::vector<char> buffer(1024);
      auto read = WaitAwaitable(executor_, socket.read(buffer));
      if (!read.ok() || *read == 0)
        return received;
      received.insert(received.end(), buffer.begin(), buffer.begin() + *read);
    }
  }

  ReverseConnector& StartConnector(std::string server_uri = kServerUri) {
    auto connector = std::make_unique<ReverseConnector>(ReverseConnectorContext{
        .executor = any_executor_,
        .transport_factory = factory_,
        .client_url = client_url_,
        .hello = {.server_uri = std::move(server_uri),
                  .endpoint_url = kEndpointUrl},
        // Stands in for binary::Server::ServeConnection: reads until the
        // client closes, recording what it sent.
        .serve = [this](transport::any_transport socket) -> Awaitable<void> {
          ++served_;
          for (;;) {
            std::vector<char> buffer(1024);
            auto read = co_await socket.read(buffer);
            if (!read.ok() || *read == 0)
              break;
            served_bytes_.insert(served_bytes_.end(), buffer.begin(),
                                 buffer.begin() + *read);
          }
          ++closed_;
        },
        .retry_delay = 50ms,
        .min_backoff = 10ms,
        .stable_lifetime = 10s,
    });
    connector->Start();
    connectors_.push_back(std::move(connector));
    return *connectors_.back();
  }

  TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  transport::TransportFactoryImpl factory_;
  ReverseConnectPool pool_;
  ReverseConnectClock::duration hello_timeout_ = 10s;
  std::optional<ReverseConnectListener> listener_;
  std::string client_url_;
  std::vector<std::string> recognized_;
  std::vector<std::unique_ptr<ReverseConnector>> connectors_;
  int served_ = 0;
  int closed_ = 0;
  std::vector<char> served_bytes_;
};

TEST_F(ReverseConnectTcpTest, TheConnectorKeepsOneIdleSocketParked) {
  auto& connector = StartConnector();
  ASSERT_TRUE(RunUntil(executor_, [&] { return pool_.idle_count(kKey) == 1; }));
  EXPECT_EQ(recognized_, std::vector<std::string>{kKey});
  EXPECT_TRUE(pool_.ServesByReverse(kKey));
  EXPECT_EQ(connector.engaged_count(), 0u);
}

TEST_F(ReverseConnectTcpTest, TakingTheIdleSocketMakesTheConnectorDialAnother) {
  auto& connector = StartConnector();
  ASSERT_TRUE(RunUntil(executor_, [&] { return pool_.idle_count(kKey) == 1; }));

  auto taken = WaitAwaitable(
      executor_,
      ReverseConnectPool::Take(pool_.MakeTicket(any_executor_, kKey), 1s));
  ASSERT_TRUE(taken.ok());
  // The client's Hello is the first thing it sends on a taken socket.
  const auto hello = EncodeHelloMessage({.receive_buffer_size = 65535,
                                         .send_buffer_size = 65535,
                                         .endpoint_url = kEndpointUrl});
  Send(*taken, hello);

  ASSERT_TRUE(RunUntil(executor_, [&] {
    return connector.engaged_count() == 1 && pool_.idle_count(kKey) == 1;
  }));
  EXPECT_EQ(served_bytes_, hello);

  // When the client closes the taken socket the connection ends.
  taken->reset();
  ASSERT_TRUE(
      RunUntil(executor_, [&] { return connector.engaged_count() == 0; }));
}

TEST_F(ReverseConnectTcpTest, CloseWaitsForEveryServedConnectionToEnd) {
  auto& connector = StartConnector();
  ASSERT_TRUE(RunUntil(executor_, [&] { return pool_.idle_count(kKey) == 1; }));
  auto taken = WaitAwaitable(
      executor_,
      ReverseConnectPool::Take(pool_.MakeTicket(any_executor_, kKey), 1s));
  ASSERT_TRUE(taken.ok());
  Send(*taken, {'H', 'E', 'L', 'F'});
  ASSERT_TRUE(
      RunUntil(executor_, [&] { return connector.engaged_count() == 1; }));

  // The client keeps its end open; Close() ends the served connection itself.
  WaitAwaitable(executor_, connector.Close());
  EXPECT_EQ(served_, closed_);
  EXPECT_EQ(connector.engaged_count(), 0u);
}

TEST_F(ReverseConnectTcpTest, AnUnrecognizedServerIsRefusedWithAnError) {
  auto socket = Dial();
  Send(socket, EncodeReverseHelloMessage({.server_uri = "urn:stranger",
                                          .endpoint_url = kEndpointUrl}));
  const auto error = DecodeErrorMessage(ReadToEnd(socket));
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->error.code(), StatusCode::Bad_TcpEndpointUrlInvalid);
  EXPECT_EQ(pool_.idle_count(kKey), 0u);
  EXPECT_FALSE(pool_.ServesByReverse(kKey));
}

TEST_F(ReverseConnectTcpTest, AHelloInsteadOfAReverseHelloIsRefused) {
  auto socket = Dial();
  Send(socket, EncodeHelloMessage({.receive_buffer_size = 65535,
                                   .send_buffer_size = 65535,
                                   .endpoint_url = kEndpointUrl}));
  const auto error = DecodeErrorMessage(ReadToEnd(socket));
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->error.code(), StatusCode::Bad_TcpEndpointUrlInvalid);
  EXPECT_TRUE(recognized_.empty());
}

TEST_F(ReverseConnectTcpTest, AReverseHelloOverTheSizeBoundIsRefused) {
  auto socket = Dial();
  Send(socket, EncodeReverseHelloMessage({.server_uri = std::string(4096, 'u'),
                                          .endpoint_url = kEndpointUrl}));
  const auto error = DecodeErrorMessage(ReadToEnd(socket));
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->error.code(), StatusCode::Bad_TcpEndpointUrlInvalid);
  EXPECT_TRUE(recognized_.empty());
}

// §7.1.3 Table 78: "Client shall close the socket after short delay if no
// ReverseHello Message received".
TEST_F(ReverseConnectTcpTest, ASilentPeerIsDisconnected) {
  hello_timeout_ = 50ms;
  TearDown();
  SetUp();
  auto socket = Dial();
  EXPECT_TRUE(ReadToEnd(socket).empty());
  EXPECT_TRUE(recognized_.empty());
}

// §7.1.3 Table 78: on an Error Message the Server "shall close the socket, log
// the error and reconnects after a delay specified in its configuration".
TEST_F(ReverseConnectTcpTest, AConnectorTheClientRefusesRedialsAfterItsDelay) {
  StartConnector("urn:stranger");
  // Each refusal is one recognizer call; a second one means it came back.
  ASSERT_TRUE(RunUntil(executor_, [&] { return recognized_.size() >= 2; }));
  EXPECT_EQ(pool_.idle_count(kKey), 0u);
  // The Error was consumed, not served as data.
  EXPECT_TRUE(served_bytes_.empty());
}

TEST_F(ReverseConnectTcpTest, TheFactoryDialsAnEndpointThatIsNotReverse) {
  ReverseConnectTransportFactory factory{factory_, pool_};
  transport::TransportString ts;
  ts.SetProtocol(transport::TransportString::TCP);
  ts.SetActive(true);
  ts.SetParam(transport::TransportString::kParamHost, "gw.example");
  ts.SetParam(transport::TransportString::kParamPort, 4844);
  auto dialled = factory.CreateTransport(ts, any_executor_);
  ASSERT_TRUE(dialled.ok());
  EXPECT_EQ(dialled->name().find("OPC UA reverse"), std::string::npos);

  pool_.Pin(kKey);
  auto reverse = factory.CreateTransport(ts, any_executor_);
  ASSERT_TRUE(reverse.ok());
  EXPECT_NE(reverse->name().find("OPC UA reverse"), std::string::npos);
}

TEST_F(ReverseConnectTcpTest, AReverseTransportOpensOnTheParkedSocket) {
  StartConnector();
  ASSERT_TRUE(RunUntil(executor_, [&] { return pool_.idle_count(kKey) == 1; }));

  ReverseConnectTransportFactory factory{factory_, pool_, 1s};
  transport::TransportString ts;
  ts.SetProtocol(transport::TransportString::TCP);
  ts.SetActive(true);
  ts.SetParam(transport::TransportString::kParamHost, "GW.example");
  ts.SetParam(transport::TransportString::kParamPort, 4844);
  auto created = factory.CreateTransport(ts, any_executor_);
  ASSERT_TRUE(created.ok());
  auto transport = std::move(created.value());
  ASSERT_EQ(WaitAwaitable(executor_, transport.open()), transport::OK);
  EXPECT_TRUE(transport.connected());

  const std::vector<char> bytes{'H', 'E', 'L', 'F'};
  Send(transport, bytes);
  ASSERT_TRUE(RunUntil(executor_,
                       [&] { return served_bytes_.size() == bytes.size(); }));
  EXPECT_EQ(served_bytes_, bytes);
}

TEST_F(ReverseConnectTcpTest, AReverseTransportWithNothingParkedTimesOut) {
  pool_.Pin(kKey);
  ReverseConnectTransportFactory factory{factory_, pool_, 20ms};
  transport::TransportString ts;
  ts.SetProtocol(transport::TransportString::TCP);
  ts.SetActive(true);
  ts.SetParam(transport::TransportString::kParamHost, "gw.example");
  ts.SetParam(transport::TransportString::kParamPort, 4844);
  auto created = factory.CreateTransport(ts, any_executor_);
  ASSERT_TRUE(created.ok());
  EXPECT_EQ(WaitAwaitable(executor_, created->open()),
            transport::ERR_TIMED_OUT);
}

}  // namespace
}  // namespace opcua::binary
