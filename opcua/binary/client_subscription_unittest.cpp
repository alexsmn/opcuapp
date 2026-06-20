#include "opcua/client_protocol_subscription.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/client_channel.h"
#include "opcua/binary/client_connection.h"
#include "opcua/binary/client_secure_channel.h"
#include "opcua/binary/client_transport.h"
#include "opcua/binary/secure_channel.h"
#include "opcua/binary/service_codec.h"
#include "transport/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace opcua::binary {
namespace {

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

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

constexpr std::uint32_t kChannelId = 42;
constexpr std::uint32_t kTokenId = 1;

std::vector<char> BuildOpenResponseFrame() {
  const OpenSecureChannelResponse response{
      .response_header = {.request_handle = 1,
                          .service_result = opcua::scada::StatusCode::Good},
      .server_protocol_version = 0,
      .security_token = {.channel_id = kChannelId,
                         .token_id = kTokenId,
                         .created_at = 0,
                         .revised_lifetime = 60000},
      .server_nonce = {},
  };
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureOpen,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kChannelId,
      .asymmetric_security_header = AsymmetricSecurityHeader{
          .security_policy_uri = std::string{kSecurityPolicyNone},
          .sender_certificate = {},
          .receiver_certificate_thumbprint = {},
      },
      .sequence_header = {.sequence_number = 1, .request_id = 1},
      .body = EncodeOpenSecureChannelResponseBody(response),
  };
  return EncodeSecureConversationMessage(message);
}

std::vector<char> BuildServiceResponseFrame(std::uint32_t request_id,
                                            std::uint32_t request_handle,
                                            ResponseBody body) {
  const auto encoded =
      EncodeServiceResponse(request_handle, std::move(body));
  const SecureConversationMessage message{
      .frame_header = {.message_type = MessageType::SecureMessage,
                       .chunk_type = 'F',
                       .message_size = 0},
      .secure_channel_id = kChannelId,
      .asymmetric_security_header = std::nullopt,
      .symmetric_security_header =
          SymmetricSecurityHeader{.token_id = kTokenId},
      .sequence_header = {.sequence_number = request_id + 1,
                          .request_id = request_id},
      .body = encoded.value(),
  };
  return EncodeSecureConversationMessage(message);
}

class ClientProtocolSubscriptionTest : public ::testing::Test {
 protected:
  // Opens the transport + secure channel, leaving request_id counters
  // positioned at 2 (next client request) and request_handle at 1.
  void OpenChannel(const std::shared_ptr<ScriptedState>& state,
                   ClientTransport& transport,
                   ClientSecureChannel& secure_channel) {
    state->incoming.push_front(AsString(BuildOpenResponseFrame()));
    state->incoming.push_front(AsString(EncodeAcknowledgeMessage(
        {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
    ASSERT_TRUE(opcua::WaitAwaitable(executor_, transport.Connect()).good());
    ASSERT_TRUE(opcua::WaitAwaitable(executor_, secure_channel.Open()).good());
  }

  std::unique_ptr<ClientTransport> MakeClientTransport(
      const std::shared_ptr<ScriptedState>& state) {
    return std::make_unique<ClientTransport>(
        ClientTransportContext{
            .transport = transport::any_transport{
                ScriptedTransport{any_executor_, state}},
            .endpoint_url = "opc.tcp://localhost:4840",
            .limits = {},
        });
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(ClientProtocolSubscriptionTest, CreateCapturesSubscriptionId) {
  auto state = std::make_shared<ScriptedState>();
  // Create response: request_id=2, request_handle=1.
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSubscriptionResponse{
          .status = opcua::scada::StatusCode::Good,
          .subscription_id = 99,
          .revised_publishing_interval_ms = 500.0,
          .revised_lifetime_count = 1200,
          .revised_max_keep_alive_count = 20}})));

  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  const auto status = opcua::WaitAwaitable(executor_, subscription.Create());
  EXPECT_TRUE(status.good());
  EXPECT_TRUE(subscription.is_created());
  EXPECT_EQ(subscription.subscription_id(), 99u);
}

TEST_F(ClientProtocolSubscriptionTest,
       CreateMonitoredItemReturnsServerItemId) {
  auto state = std::make_shared<ScriptedState>();
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSubscriptionResponse{
          .status = opcua::scada::StatusCode::Good, .subscription_id = 99}})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/3, /*request_handle=*/2,
      ResponseBody{CreateMonitoredItemsResponse{
          .status = opcua::scada::StatusCode::Good,
          .results = {MonitoredItemCreateResult{
              .status = opcua::scada::StatusCode::Good,
              .monitored_item_id = 101,
              .revised_sampling_interval_ms = 500.0,
              .revised_queue_size = 1,
          }}}})));

  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Create()).good());

  const auto result = opcua::WaitAwaitable(
      executor_,
      subscription.CreateMonitoredItem(
          opcua::scada::ReadValueId{.node_id = opcua::scada::NodeId{1},
                              .attribute_id = opcua::scada::AttributeId::Value},
          {}, [](opcua::scada::DataValue) {}));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->monitored_item_id, 101u);
  EXPECT_EQ(result->client_handle, 1u);
}

TEST_F(ClientProtocolSubscriptionTest,
       PublishDispatchesNotificationsToHandlers) {
  auto state = std::make_shared<ScriptedState>();
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/2, /*request_handle=*/1,
      ResponseBody{CreateSubscriptionResponse{
          .status = opcua::scada::StatusCode::Good, .subscription_id = 1}})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/3, /*request_handle=*/2,
      ResponseBody{CreateMonitoredItemsResponse{
          .status = opcua::scada::StatusCode::Good,
          .results = {MonitoredItemCreateResult{
              .status = opcua::scada::StatusCode::Good,
              .monitored_item_id = 555,
              .revised_sampling_interval_ms = 500.0,
              .revised_queue_size = 1,
          }}}})));
  // Publish response carries a data-change notification whose client_handle
  // matches the one the subscription assigned to the monitored item (1).
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      /*request_id=*/4, /*request_handle=*/3,
      ResponseBody{PublishResponse{
          .status = opcua::scada::StatusCode::Good,
          .subscription_id = 1,
          .more_notifications = false,
          .notification_message =
              {.sequence_number = 10,
               .notification_data =
                   {DataChangeNotification{
                       .monitored_items = {MonitoredItemNotification{
                           .client_handle = 1,
                           .value = opcua::scada::DataValue{
                               opcua::scada::Variant{std::int32_t{88}},
                               {}, {}, {}}}}}}},
      }})));

  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Create()).good());

  std::vector<opcua::scada::DataValue> received;
  ASSERT_TRUE(
      opcua::WaitAwaitable(
          executor_,
          subscription.CreateMonitoredItem(
              opcua::scada::ReadValueId{.node_id = opcua::scada::NodeId{1},
                                  .attribute_id = opcua::scada::AttributeId::Value},
              {}, [&](opcua::scada::DataValue v) { received.push_back(v); }))
          .ok());

  const auto status = opcua::WaitAwaitable(executor_, subscription.Publish());
  ASSERT_TRUE(status.good());
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].value, (opcua::scada::Variant{std::int32_t{88}}));

  // CreateSubscription + CreateMonitoredItem are writes[2] and writes[3].
  // The first Publish() call fills a two-request Publish window before it
  // waits for the first response.
  ASSERT_GE(state->writes.size(), 6u);
  const auto first_publish_bytes = std::vector<char>{
      state->writes[4].begin(), state->writes[4].end()};
  const auto second_publish_bytes = std::vector<char>{
      state->writes[5].begin(), state->writes[5].end()};
  const auto first_message = DecodeSecureConversationMessage(first_publish_bytes);
  const auto second_message =
      DecodeSecureConversationMessage(second_publish_bytes);
  ASSERT_TRUE(first_message.has_value());
  ASSERT_TRUE(second_message.has_value());
  const auto first_request =
      DecodeServiceRequest(first_message->body);
  const auto second_request =
      DecodeServiceRequest(second_message->body);
  ASSERT_TRUE(first_request.has_value());
  ASSERT_TRUE(second_request.has_value());
  EXPECT_NE(std::get_if<PublishRequest>(&first_request->body), nullptr);
  EXPECT_NE(std::get_if<PublishRequest>(&second_request->body), nullptr);
}

TEST_F(ClientProtocolSubscriptionTest, PublishAcksPriorSequenceNumber) {
  auto state = std::make_shared<ScriptedState>();
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      2, 1,
      ResponseBody{CreateSubscriptionResponse{
          .status = opcua::scada::StatusCode::Good, .subscription_id = 1}})));
  // First Publish response: sequence_number 7.
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      3, 2,
      ResponseBody{PublishResponse{
          .status = opcua::scada::StatusCode::Good,
          .subscription_id = 1,
          .notification_message = {.sequence_number = 7}}})));
  // Second Publish response: sequence_number 8.
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      4, 3,
      ResponseBody{PublishResponse{
          .status = opcua::scada::StatusCode::Good,
          .subscription_id = 1,
          .notification_message = {.sequence_number = 8}}})));

  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Create()).good());
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Publish()).good());
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Publish()).good());

  // Publish() now keeps two requests outstanding. The first two Publish
  // requests are sent before any response is received; the third carries
  // the ack for the first response's sequence_number (7).
  ASSERT_GE(state->writes.size(), 6u);
  const auto second_publish_bytes = std::vector<char>{
      state->writes[5].begin(), state->writes[5].end()};
  const auto message = DecodeSecureConversationMessage(second_publish_bytes);
  ASSERT_TRUE(message.has_value());
  const auto decoded_request = DecodeServiceRequest(message->body);
  ASSERT_TRUE(decoded_request.has_value());
  const auto* publish_request =
      std::get_if<PublishRequest>(&decoded_request->body);
  ASSERT_NE(publish_request, nullptr);
  ASSERT_EQ(publish_request->subscription_acknowledgements.size(), 1u);
  EXPECT_EQ(publish_request->subscription_acknowledgements[0].subscription_id,
            1u);
  EXPECT_EQ(publish_request->subscription_acknowledgements[0].sequence_number,
            7u);
}

TEST_F(ClientProtocolSubscriptionTest, DeleteMonitoredItemDropsHandler) {
  auto state = std::make_shared<ScriptedState>();
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      2, 1,
      ResponseBody{CreateSubscriptionResponse{
          .status = opcua::scada::StatusCode::Good, .subscription_id = 1}})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      3, 2,
      ResponseBody{CreateMonitoredItemsResponse{
          .status = opcua::scada::StatusCode::Good,
          .results = {MonitoredItemCreateResult{
              .status = opcua::scada::StatusCode::Good,
              .monitored_item_id = 42,
          }}}})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      4, 3,
      ResponseBody{DeleteMonitoredItemsResponse{
          .status = opcua::scada::StatusCode::Good,
          .results = {opcua::scada::StatusCode::Good}}})));
  // After delete, a publish with a matching-client-handle notification
  // should NOT dispatch anywhere (the handler has been removed). Verified
  // by absence of side-effect.
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      5, 4,
      ResponseBody{PublishResponse{
          .status = opcua::scada::StatusCode::Good,
          .subscription_id = 1,
          .notification_message =
              {.sequence_number = 1,
               .notification_data =
                   {DataChangeNotification{
                       .monitored_items = {MonitoredItemNotification{
                           .client_handle = 1,
                           .value = opcua::scada::DataValue{
                               opcua::scada::Variant{std::int32_t{1}}, {}, {}, {}}}}}}}}})));

  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Create()).good());

  int callback_count = 0;
  ASSERT_TRUE(
      opcua::WaitAwaitable(
          executor_,
          subscription.CreateMonitoredItem(
              opcua::scada::ReadValueId{.node_id = opcua::scada::NodeId{1},
                                  .attribute_id = opcua::scada::AttributeId::Value},
              {}, [&](opcua::scada::DataValue) { ++callback_count; }))
          .ok());

  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, subscription.DeleteMonitoredItem(42)).good());
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Publish()).good());
  EXPECT_EQ(callback_count, 0);
}

TEST_F(ClientProtocolSubscriptionTest, DeleteClearsServerSubscription) {
  auto state = std::make_shared<ScriptedState>();
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      2, 1,
      ResponseBody{CreateSubscriptionResponse{
          .status = opcua::scada::StatusCode::Good, .subscription_id = 1}})));
  state->incoming.push_back(AsString(BuildServiceResponseFrame(
      3, 2,
      ResponseBody{DeleteSubscriptionsResponse{
          .status = opcua::scada::StatusCode::Good,
          .results = {opcua::scada::StatusCode::Good}}})));

  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Create()).good());
  ASSERT_TRUE(opcua::WaitAwaitable(executor_, subscription.Delete()).good());
  EXPECT_FALSE(subscription.is_created());
}

TEST_F(ClientProtocolSubscriptionTest,
       CreateMonitoredItemBeforeCreateReturnsBad) {
  auto state = std::make_shared<ScriptedState>();
  auto transport = MakeClientTransport(state);
  ClientSecureChannel secure_channel{*transport};
  ClientConnection connection{
      {.transport = *transport, .secure_channel = secure_channel}};
  ClientChannel channel{{.executor = any_executor_, .connection = connection}};
  OpenChannel(state, *transport, secure_channel);

  ClientProtocolSubscription subscription{channel};
  const auto result = opcua::WaitAwaitable(
      executor_,
      subscription.CreateMonitoredItem(
          opcua::scada::ReadValueId{.node_id = opcua::scada::NodeId{1}}, {},
          [](opcua::scada::DataValue) {}));
  EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace opcua::binary
