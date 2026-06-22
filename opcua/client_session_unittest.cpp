#include "opcua/client_session.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/service_codec.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/legacy_monitored_item_adapter.h"
#include "opcua/scada/monitored_item.h"
#include "opcua/scada/test/status_matchers.h"
#include "opcua/scada/variant.h"
#include "transport/transport_factory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using opcua::ClientSession;

class UnusedTransportFactory final : public transport::TransportFactory {
 public:
  transport::expected<transport::any_transport> CreateTransport(
      const transport::TransportString& /*transport_string*/,
      const transport::executor& /*executor*/,
      const transport::log_source& /*log*/) override {
    ADD_FAILURE() << "Unexpected transport construction";
    return transport::ERR_INVALID_ARGUMENT;
  }
};

struct ScriptedState {
  std::deque<std::string> incoming;
  std::vector<std::string> writes;
  bool opened = false;
  bool closed = false;
};

class ScriptedTransport {
 public:
  ScriptedTransport(transport::executor executor,
                    std::shared_ptr<ScriptedState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  ScriptedTransport(ScriptedTransport&&) = default;
  ScriptedTransport& operator=(ScriptedTransport&&) = default;
  ScriptedTransport(const ScriptedTransport&) = delete;
  ScriptedTransport& operator=(const ScriptedTransport&) = delete;

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

  std::string name() const { return "ScriptedTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<ScriptedState> state_;
};

class ScriptedTransportFactory final : public transport::TransportFactory {
 public:
  explicit ScriptedTransportFactory(std::shared_ptr<ScriptedState> state)
      : state_{std::move(state)} {}

  transport::expected<transport::any_transport> CreateTransport(
      const transport::TransportString& /*transport_string*/,
      const transport::executor& executor,
      const transport::log_source& /*log*/) override {
    return transport::any_transport{ScriptedTransport{executor, state_}};
  }

 private:
  std::shared_ptr<ScriptedState> state_;
};

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

constexpr std::uint32_t kChannelId = 42;
constexpr std::uint32_t kTokenId = 1;

std::vector<char> BuildOpenResponseFrame() {
  const opcua::binary::OpenSecureChannelResponse response{
      .response_header = {.request_handle = 1,
                          .service_result = opcua::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = kChannelId,
                         .token_id = kTokenId,
                         .created_at = 0,
                         .revised_lifetime = 60000},
      .server_nonce = {},
  };
  const opcua::binary::SecureConversationMessage message{
      .frame_header = {.message_type = opcua::binary::MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kChannelId,
      .asymmetric_security_header =
          opcua::binary::AsymmetricSecurityHeader{
              .security_policy_uri =
                  std::string{opcua::binary::kSecurityPolicyNone},
              .sender_certificate = {},
              .receiver_certificate_thumbprint = {},
          },
      .sequence_header = {.sequence_number = 1, .request_id = 1},
      .body = opcua::binary::EncodeOpenSecureChannelResponseBody(response),
  };
  return opcua::binary::EncodeSecureConversationMessage(message);
}

std::vector<char> BuildServiceResponseFrame(std::uint32_t request_id,
                                            std::uint32_t request_handle,
                                            opcua::ResponseBody body) {
  const auto encoded =
      opcua::binary::EncodeServiceResponse(request_handle, std::move(body));
  const opcua::binary::SecureConversationMessage message{
      .frame_header = {.message_type =
                           opcua::binary::MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kChannelId,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          opcua::binary::SymmetricSecurityHeader{.token_id = kTokenId},
      .sequence_header = {.sequence_number = request_id + 1,
                          .request_id = request_id},
      .body = encoded.value(),
  };
  return opcua::binary::EncodeSecureConversationMessage(message);
}

void PrimeConnectAndOpen(const std::shared_ptr<ScriptedState>& state) {
  state->incoming.push_back(AsString(opcua::binary::EncodeAcknowledgeMessage(
      {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
  state->incoming.push_back(AsString(BuildOpenResponseFrame()));
}

void PrimeSessionEstablishment(const std::shared_ptr<ScriptedState>& state) {
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      opcua::ResponseBody{opcua::CreateSessionResponse{
          .status = opcua::StatusCode::Good,
          .session_id = opcua::NodeId{111},
          .authentication_token = opcua::NodeId{222},
          .server_nonce = opcua::ByteString{},
          .revised_timeout = opcua::base::TimeDelta::FromSeconds(60),
      }})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/3, /*request_handle=*/2,
      opcua::ResponseBody{
          opcua::ActivateSessionResponse{.status = opcua::StatusCode::Good}})));
}

// After ActivateSession (request_id 3) the session reads Server_NamespaceArray;
// this is request_id 4 / request_handle 3.
void PrimeNamespaceArray(const std::shared_ptr<ScriptedState>& state) {
  opcua::DataValue value;
  value.value = opcua::Variant{std::vector<std::string>{
      "http://opcfoundation.org/UA/", "http://telecontrol.ru/opcua/scada"}};
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      opcua::ResponseBody{opcua::ReadResponse{
          .status = opcua::StatusCode::Good, .results = {std::move(value)}}})));
}

// Subscription creation follows the namespace-array read, so its requests are
// request_id 5 / 6 (request_handle 4 / 5).
void PrimeSubscriptionCreation(const std::shared_ptr<ScriptedState>& state) {
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/5, /*request_handle=*/4,
      opcua::ResponseBody{opcua::CreateSubscriptionResponse{
          .status = opcua::StatusCode::Good,
          .subscription_id = 77,
          .revised_publishing_interval_ms = 500.0,
          .revised_lifetime_count = 1200,
          .revised_max_keep_alive_count = 20}})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/6, /*request_handle=*/5,
      opcua::ResponseBody{opcua::CreateMonitoredItemsResponse{
          .status = opcua::StatusCode::Good,
          .results = {opcua::MonitoredItemCreateResult{
              .status = opcua::StatusCode::Good,
              .monitored_item_id = 101,
              .revised_sampling_interval_ms = 250.0,
              .revised_queue_size = 1,
          }}}})));
}

std::vector<opcua::RequestBody> DecodeServiceRequests(
    const std::vector<std::string>& writes) {
  std::vector<opcua::RequestBody> requests;
  for (const auto& write : writes) {
    const std::vector<char> bytes{write.begin(), write.end()};
    auto message = opcua::binary::DecodeSecureConversationMessage(bytes);
    if (!message) {
      continue;
    }
    auto request = opcua::binary::DecodeServiceRequest(message->body);
    if (request) {
      requests.push_back(std::move(request->body));
    }
  }
  return requests;
}

class ClientSessionTest : public ::testing::Test {
 protected:
  opcua::TestExecutor executor_;
  UnusedTransportFactory transport_factory_;
};

TEST_F(ClientSessionTest, InvalidEndpointRejectsAwaitableWithStatus) {
  auto session = std::make_shared<ClientSession>(executor_, transport_factory_);

  auto status = opcua::WaitAwaitable(
      executor_, session->ConnectAsync({.connection_string = "http://host"}));
  EXPECT_EQ(status.code(), opcua::StatusCode::Bad);
}

TEST_F(ClientSessionTest, InvalidEndpointRejectsConnectWithStatus) {
  auto session = std::make_shared<ClientSession>(executor_, transport_factory_);

  auto status = opcua::WaitAwaitable(
      executor_, session->ConnectStatus({.connection_string = "http://host"}));
  EXPECT_EQ(status.code(), opcua::StatusCode::Bad);
}

TEST_F(ClientSessionTest, SessionServiceRejectsInvalidEndpoint) {
  auto session = std::make_shared<ClientSession>(executor_, transport_factory_);
  auto& coroutine_session = *session;

  auto status = opcua::WaitAwaitable(
      executor_,
      coroutine_session.ConnectStatus({.connection_string = "http://host"}));
  EXPECT_EQ(status.code(), opcua::StatusCode::Bad);
}

TEST_F(ClientSessionTest, SessionServiceReportsDisconnectedMetadata) {
  auto session = std::make_shared<ClientSession>(executor_, transport_factory_);
  auto& coroutine_session = *session;

  opcua::base::TimeDelta ping_delay;
  EXPECT_FALSE(coroutine_session.IsConnected(&ping_delay));
  EXPECT_TRUE(ping_delay.is_zero());
  EXPECT_TRUE(coroutine_session.HasPrivilege(opcua::Privilege::Configure));
  EXPECT_FALSE(coroutine_session.IsScada());
  EXPECT_EQ(coroutine_session.GetUserId(), opcua::NodeId{});
  EXPECT_EQ(coroutine_session.GetHostName(), "");
  EXPECT_EQ(coroutine_session.GetSessionDebugger(), nullptr);

  EXPECT_NO_THROW(opcua::WaitAwaitable(executor_, coroutine_session.Reconnect()));
  EXPECT_NO_THROW(opcua::WaitAwaitable(executor_, coroutine_session.Disconnect()));
}

TEST_F(ClientSessionTest, SessionServiceConnectsClientSession) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  PrimeNamespaceArray(state);

  ScriptedTransportFactory transport_factory{state};
  auto session = std::make_shared<ClientSession>(executor_, transport_factory);
  auto& coroutine_session = *session;

  bool state_changed = false;
  auto subscription = coroutine_session.SubscribeSessionStateChanged(
      [&](bool connected, const opcua::Status& status) {
        state_changed = true;
        EXPECT_TRUE(connected);
        EXPECT_EQ(status.code(), opcua::StatusCode::Good);
      });

  ASSERT_NO_THROW(opcua::WaitAwaitable(
      executor_, coroutine_session.Connect({.host = "localhost:4840"})));

  EXPECT_TRUE(state_changed);
  EXPECT_TRUE(coroutine_session.IsConnected());
  EXPECT_TRUE(session->IsConnected());

  const auto requests = DecodeServiceRequests(state->writes);
  EXPECT_NE(
      std::ranges::find_if(
          requests,
          [](const opcua::RequestBody& body) {
            return std::holds_alternative<opcua::CreateSessionRequest>(body);
          }),
      requests.end());
  EXPECT_NE(
      std::ranges::find_if(
          requests,
          [](const opcua::RequestBody& body) {
            return std::holds_alternative<opcua::ActivateSessionRequest>(body);
          }),
      requests.end());
}

TEST_F(ClientSessionTest, ConnectReadsServerNamespaceArray) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  PrimeNamespaceArray(state);

  ScriptedTransportFactory transport_factory{state};
  auto session = std::make_shared<ClientSession>(executor_, transport_factory);

  ASSERT_NO_THROW(
      opcua::WaitAwaitable(executor_, session->Connect({.host = "localhost:4840"})));

  const auto& table = session->namespace_table();
  ASSERT_EQ(table.size(), 2u);
  EXPECT_EQ(table.UriForIndex(0), "http://opcfoundation.org/UA/");
  EXPECT_EQ(table.IndexForUri("http://telecontrol.ru/opcua/scada"), 1);
}

TEST_F(ClientSessionTest, AwaitableServicesReportDisconnected) {
  auto session = std::make_shared<ClientSession>(executor_, transport_factory_);

  auto read_inputs = std::make_shared<const std::vector<opcua::ReadValueId>>(
      std::vector<opcua::ReadValueId>{});
  auto read_result = opcua::WaitAwaitable(executor_, session->Read({}, read_inputs));
  EXPECT_THAT(read_result,
              opcua::test::StatusIs(opcua::StatusCode::Bad_Disconnected));

  auto write_inputs = std::make_shared<const std::vector<opcua::WriteValue>>(
      std::vector<opcua::WriteValue>{});
  auto write_result =
      opcua::WaitAwaitable(executor_, session->Write({}, write_inputs));
  EXPECT_THAT(write_result,
              opcua::test::StatusIs(opcua::StatusCode::Bad_Disconnected));

  auto browse_result = opcua::WaitAwaitable(executor_, session->Browse({}, {}));
  EXPECT_THAT(browse_result,
              opcua::test::StatusIs(opcua::StatusCode::Bad_Disconnected));

  auto translate_result =
      opcua::WaitAwaitable(executor_, session->TranslateBrowsePaths({}));
  EXPECT_THAT(translate_result,
              opcua::test::StatusIs(opcua::StatusCode::Bad_Disconnected));

  auto call_status = opcua::WaitAwaitable(executor_, session->Call({}, {}, {}, {}));
  EXPECT_EQ(call_status.code(), opcua::StatusCode::Bad_Disconnected);
}

TEST_F(ClientSessionTest, MonitoredItemUsesSubscriptionCoroutineTasks) {
  auto state = std::make_shared<ScriptedState>();
  PrimeConnectAndOpen(state);
  PrimeSessionEstablishment(state);
  PrimeNamespaceArray(state);
  PrimeSubscriptionCreation(state);

  ScriptedTransportFactory transport_factory{state};
  auto session = std::make_shared<ClientSession>(executor_, transport_factory);

  ASSERT_NO_THROW(
      opcua::WaitAwaitable(executor_, session->Connect({.host = "localhost:4840"})));

  opcua::scada::LegacyMonitoredItemAdapter monitored_item_adapter{executor_, *session};
  auto monitored_item = monitored_item_adapter.CreateMonitoredItem(
      opcua::ReadValueId{.node_id = opcua::NodeId{1},
                         .attribute_id = opcua::AttributeId::Value},
      opcua::MonitoringParameters{.sampling_interval_ms = 250,
                                  .queue_size = 1});
  monitored_item->Subscribe(
      static_cast<opcua::scada::DataChangeHandler>([](opcua::DataValue) {}));
  Drain(executor_);

  monitored_item.reset();
  Drain(executor_);

  const auto requests = DecodeServiceRequests(state->writes);
  EXPECT_NE(
      std::ranges::find_if(
          requests,
          [](const opcua::RequestBody& body) {
            return std::holds_alternative<opcua::CreateSubscriptionRequest>(
                body);
          }),
      requests.end());
  EXPECT_NE(
      std::ranges::find_if(
          requests,
          [](const opcua::RequestBody& body) {
            return std::holds_alternative<opcua::CreateMonitoredItemsRequest>(
                body);
          }),
      requests.end());
  EXPECT_NE(
      std::ranges::find_if(
          requests,
          [](const opcua::RequestBody& body) {
            return std::holds_alternative<opcua::DeleteMonitoredItemsRequest>(
                body);
          }),
      requests.end());
}

}  // namespace
