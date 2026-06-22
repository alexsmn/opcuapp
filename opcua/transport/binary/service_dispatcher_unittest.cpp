#include "opcua/transport/binary/service_dispatcher.h"
#include "opcua/transport/binary/codec_utils.h"
#include "opcua/transport/binary/service_codec.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/base/time_utils.h"
#include "opcua/monitored/item_factory_subscription.h"
#include "opcua/monitored/test/test_monitored_item.h"
#include "opcua/server/endpoint_core.h"
#include "opcua/session/authentication_adapters.h"
#include "opcua/transport/binary/protocol.h"
#include "opcua/transport/binary/secure_channel.h"
#include "opcua/transport/binary/tcp_connection.h"
#include "transport/transport.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <deque>

using namespace testing;

namespace opcua::binary {
namespace {

constexpr std::uint32_t kCreateSessionRequestEncodingId = 461;
constexpr std::uint32_t kCreateSessionResponseEncodingId = 464;
constexpr std::uint32_t kActivateSessionRequestEncodingId = 467;
constexpr std::uint32_t kActivateSessionResponseEncodingId = 470;
constexpr std::uint32_t kCloseSessionRequestEncodingId = 473;
constexpr std::uint32_t kCloseSessionResponseEncodingId = 476;
constexpr std::uint32_t kAddNodesRequestEncodingId = 488;
constexpr std::uint32_t kAddNodesResponseEncodingId = 491;
constexpr std::uint32_t kAddReferencesRequestEncodingId = 494;
constexpr std::uint32_t kAddReferencesResponseEncodingId = 497;
constexpr std::uint32_t kDeleteNodesRequestEncodingId = 500;
constexpr std::uint32_t kDeleteNodesResponseEncodingId = 503;
constexpr std::uint32_t kDeleteReferencesRequestEncodingId = 506;
constexpr std::uint32_t kDeleteReferencesResponseEncodingId = 509;
constexpr std::uint32_t kBrowseRequestEncodingId = 525;
constexpr std::uint32_t kBrowseResponseEncodingId = 528;
constexpr std::uint32_t kBrowseNextRequestEncodingId = 533;
constexpr std::uint32_t kBrowseNextResponseEncodingId = 536;
constexpr std::uint32_t kCreateSubscriptionRequestEncodingId = 787;
constexpr std::uint32_t kCreateSubscriptionResponseEncodingId = 790;
constexpr std::uint32_t kModifySubscriptionRequestEncodingId = 793;
constexpr std::uint32_t kModifySubscriptionResponseEncodingId = 796;
constexpr std::uint32_t kSetPublishingModeRequestEncodingId = 799;
constexpr std::uint32_t kSetPublishingModeResponseEncodingId = 802;
constexpr std::uint32_t kCreateMonitoredItemsRequestEncodingId = 751;
constexpr std::uint32_t kCreateMonitoredItemsResponseEncodingId = 754;
constexpr std::uint32_t kModifyMonitoredItemsRequestEncodingId = 763;
constexpr std::uint32_t kModifyMonitoredItemsResponseEncodingId = 766;
constexpr std::uint32_t kSetMonitoringModeRequestEncodingId = 769;
constexpr std::uint32_t kSetMonitoringModeResponseEncodingId = 772;
constexpr std::uint32_t kDeleteMonitoredItemsRequestEncodingId = 781;
constexpr std::uint32_t kDeleteMonitoredItemsResponseEncodingId = 784;
constexpr std::uint32_t kDeleteSubscriptionsRequestEncodingId = 847;
constexpr std::uint32_t kDeleteSubscriptionsResponseEncodingId = 850;
constexpr std::uint32_t kPublishRequestEncodingId = 826;
constexpr std::uint32_t kPublishResponseEncodingId = 829;
constexpr std::uint32_t kRepublishRequestEncodingId = 832;
constexpr std::uint32_t kRepublishResponseEncodingId = 835;
constexpr std::uint32_t kTransferSubscriptionsRequestEncodingId = 841;
constexpr std::uint32_t kTransferSubscriptionsResponseEncodingId = 844;
constexpr std::uint32_t kTranslateBrowsePathsRequestEncodingId = 554;
constexpr std::uint32_t kTranslateBrowsePathsResponseEncodingId = 557;
constexpr std::uint32_t kReadRawModifiedDetailsEncodingId = 649;
constexpr std::uint32_t kHistoryDataEncodingId = 658;
constexpr std::uint32_t kHistoryEventEncodingId = 661;
constexpr std::uint32_t kHistoryReadRequestEncodingId = 664;
constexpr std::uint32_t kHistoryReadResponseEncodingId = 667;
constexpr std::uint32_t kCallRequestEncodingId = 710;
constexpr std::uint32_t kCallResponseEncodingId = 713;
constexpr std::uint32_t kReadRequestEncodingId = 629;
constexpr std::uint32_t kReadResponseEncodingId = 632;
constexpr std::uint32_t kWriteRequestEncodingId = 671;
constexpr std::uint32_t kWriteResponseEncodingId = 674;
constexpr std::uint32_t kUserNameIdentityTokenEncodingId = 324;

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

opcua::NodeId NumericNode(opcua::NumericId id, opcua::NamespaceIndex ns = 2) {
  return {id, ns};
}

opcua::base::Time ParseTime(std::string_view value) {
  opcua::base::Time result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

void AppendRequestHeader(std::vector<char>& bytes,
                         const opcua::NodeId& authentication_token,
                         std::uint32_t request_handle) {
  Encoder encoder{bytes};
  encoder.Encode(authentication_token);
  encoder.Encode(std::int64_t{0});
  encoder.Encode(request_handle);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(std::string_view{""});
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(opcua::NodeId{});
  encoder.Encode(std::uint8_t{0x00});
}

std::vector<char> EncodeOpenRequestBody(std::uint32_t request_handle) {
  std::vector<char> payload;
  Encoder payload_encoder{payload};
  payload_encoder.Encode(opcua::NodeId{});
  payload_encoder.Encode(std::int64_t{0});
  payload_encoder.Encode(request_handle);
  payload_encoder.Encode(std::uint32_t{0});
  payload_encoder.Encode(std::string_view{""});
  payload_encoder.Encode(std::uint32_t{0});
  payload_encoder.Encode(opcua::NodeId{});
  payload_encoder.Encode(std::uint8_t{0x00});
  payload_encoder.Encode(std::uint32_t{0});
  payload_encoder.Encode(
      static_cast<std::uint32_t>(SecurityTokenRequestType::Issue));
  payload_encoder.Encode(static_cast<std::uint32_t>(MessageSecurityMode::None));
  payload_encoder.Encode(opcua::ByteString{});
  payload_encoder.Encode(std::uint32_t{60000});

  std::vector<char> body;
  Encoder body_encoder{body};
  body_encoder.Encode(EncodedExtensionObject{
      .type_id = kOpenSecureChannelRequestEncodingId, .body = payload});
  return body;
}

std::vector<char> EncodeCreateSessionRequestBody(std::uint32_t request_handle,
                                                 double requested_timeout_ms) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = opcua::NodeId{},
       .request_handle = request_handle},
      RequestBody{CreateSessionRequest{
          .requested_timeout = opcua::base::TimeDelta::FromMillisecondsD(
              requested_timeout_ms)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeUserNameActivateRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    std::string_view user_name,
    std::string_view password) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{ActivateSessionRequest{
          .authentication_token = authentication_token,
          .user_name = opcua::ToLocalizedText(std::string{user_name}),
          .password = opcua::ToLocalizedText(std::string{password}),
          .allow_anonymous = false}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeCloseSessionRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{
          CloseSessionRequest{.authentication_token = authentication_token}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeReadRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::ReadValueId& read_value_id) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{ReadRequest{.inputs = {read_value_id}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeWriteRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::WriteValue& write_value) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{WriteRequest{.inputs = {write_value}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeBrowseRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::BrowseDescription& browse_description,
    size_t requested_max_references_per_node = 0) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{BrowseRequest{.requested_max_references_per_node =
                                    requested_max_references_per_node,
                                .inputs = {browse_description}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeBrowseNextRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    bool release_continuation_points,
    std::vector<opcua::ByteString> continuation_points) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{BrowseNextRequest{
          .release_continuation_points = release_continuation_points,
          .continuation_points = std::move(continuation_points)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeHistoryReadRawRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::HistoryReadRawDetails details) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{HistoryReadRawRequest{.details = std::move(details)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeHistoryReadEventsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::HistoryReadEventsDetails details) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{HistoryReadEventsRequest{.details = std::move(details)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeCreateSubscriptionRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const SubscriptionParameters& parameters) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{CreateSubscriptionRequest{.parameters = parameters}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeModifySubscriptionRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::UInt32 subscription_id,
    const SubscriptionParameters& parameters) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{ModifySubscriptionRequest{.subscription_id = subscription_id,
                                            .parameters = parameters}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeSetPublishingModeRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    bool publishing_enabled,
    std::vector<opcua::UInt32> subscription_ids) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{SetPublishingModeRequest{
          .publishing_enabled = publishing_enabled,
          .subscription_ids = std::move(subscription_ids)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeDeleteSubscriptionsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    std::vector<opcua::UInt32> subscription_ids) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{DeleteSubscriptionsRequest{.subscription_ids =
                                                 std::move(subscription_ids)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeCreateMonitoredItemsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::UInt32 subscription_id,
    std::vector<MonitoredItemCreateRequest> items_to_create) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{CreateMonitoredItemsRequest{
                               .subscription_id = subscription_id,
                               .items_to_create = std::move(items_to_create)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeModifyMonitoredItemsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::UInt32 subscription_id,
    std::vector<MonitoredItemModifyRequest> items_to_modify) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{ModifyMonitoredItemsRequest{
                               .subscription_id = subscription_id,
                               .items_to_modify = std::move(items_to_modify)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodePublishRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    std::vector<SubscriptionAcknowledgement> acknowledgements = {}) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{PublishRequest{.subscription_acknowledgements =
                                     std::move(acknowledgements)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeDeleteMonitoredItemsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::UInt32 subscription_id,
    std::vector<opcua::UInt32> monitored_item_ids) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{DeleteMonitoredItemsRequest{
          .subscription_id = subscription_id,
          .monitored_item_ids = std::move(monitored_item_ids)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeSetMonitoringModeRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::UInt32 subscription_id,
    MonitoringMode monitoring_mode,
    std::vector<opcua::UInt32> monitored_item_ids) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{SetMonitoringModeRequest{
          .subscription_id = subscription_id,
          .monitoring_mode = monitoring_mode,
          .monitored_item_ids = std::move(monitored_item_ids)}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeRepublishRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    opcua::UInt32 subscription_id,
    opcua::UInt32 retransmit_sequence_number) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{RepublishRequest{
          .subscription_id = subscription_id,
          .retransmit_sequence_number = retransmit_sequence_number}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeTransferSubscriptionsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    std::vector<opcua::UInt32> subscription_ids,
    bool send_initial_values) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{TransferSubscriptionsRequest{
                               .subscription_ids = std::move(subscription_ids),
                               .send_initial_values = send_initial_values}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeCallRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::NodeId& object_id,
    const opcua::NodeId& method_id,
    const std::vector<opcua::Variant>& arguments) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{CallRequest{.methods = {{.object_id = object_id,
                                           .method_id = method_id,
                                           .arguments = arguments}}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeTranslateBrowsePathsRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::BrowsePath& browse_path) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{TranslateBrowsePathsRequest{.inputs = {browse_path}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeDeleteNodesRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::DeleteNodesItem& item) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{DeleteNodesRequest{.items = {item}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeDeleteReferencesRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::DeleteReferencesItem& item) {
  const auto encoded = EncodeServiceRequest(
      {.authentication_token = authentication_token,
       .request_handle = request_handle},
      RequestBody{DeleteReferencesRequest{.items = {item}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeAddReferencesRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::AddReferencesItem& item) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{AddReferencesRequest{.items = {item}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::vector<char> EncodeAddNodesRequestBody(
    std::uint32_t request_handle,
    const opcua::NodeId& authentication_token,
    const opcua::AddNodesItem& item) {
  const auto encoded =
      EncodeServiceRequest({.authentication_token = authentication_token,
                            .request_handle = request_handle},
                           RequestBody{AddNodesRequest{.items = {item}}});
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::vector<char>{});
}

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

struct DecodedCreateSessionResponse {
  opcua::NodeId session_id;
  opcua::NodeId authentication_token;
};

std::optional<DecodedCreateSessionResponse> DecodeCreateSessionResponse(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kCreateSessionResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  opcua::NodeId session_id;
  opcua::NodeId authentication_token;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(session_id) ||
      !decoder.Decode(authentication_token)) {
    return std::nullopt;
  }
  return DecodedCreateSessionResponse{
      .session_id = session_id,
      .authentication_token = authentication_token,
  };
}

std::optional<std::uint32_t> DecodeResponseStatus(
    const std::vector<char>& bytes,
    std::uint32_t expected_type_id) {
  Decoder message_decoder{bytes};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != expected_type_id) {
    return std::nullopt;
  }

  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t status_code = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) || !decoder.Decode(status_code)) {
    return std::nullopt;
  }
  return status_code;
}

std::optional<double> DecodeSingleDoubleReadResponse(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kReadResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  if (!decoder.Decode(result_count) || result_count != 1) {
    return std::nullopt;
  }

  std::uint8_t data_value_mask = 0;
  if (!decoder.Decode(data_value_mask) || (data_value_mask & 0x01) == 0) {
    return std::nullopt;
  }

  std::uint8_t variant_mask = 0;
  double value = 0;
  if (!decoder.Decode(variant_mask) || variant_mask != 11 ||
      !decoder.Decode(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::uint32_t> DecodeSingleWriteResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kWriteResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

std::optional<opcua::ReferenceDescription> DecodeSingleBrowseReference(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kBrowseResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  opcua::ByteString ignored_continuation_point;
  std::int32_t reference_count = 0;
  opcua::NodeId reference_type_id;
  bool forward = false;
  opcua::ExpandedNodeId target_id;
  opcua::QualifiedName ignored_browse_name;
  opcua::LocalizedText ignored_display_name;
  std::uint32_t node_class = 0;
  opcua::ExpandedNodeId ignored_type_definition;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status) ||
      !decoder.Decode(ignored_continuation_point) ||
      !decoder.Decode(reference_count) || reference_count != 1 ||
      !decoder.Decode(reference_type_id) || !decoder.Decode(forward) ||
      !decoder.Decode(target_id) || !decoder.Decode(ignored_browse_name) ||
      !decoder.Decode(ignored_display_name) || !decoder.Decode(node_class) ||
      !decoder.Decode(ignored_type_definition)) {
    return std::nullopt;
  }

  return opcua::ReferenceDescription{
      .reference_type_id = reference_type_id,
      .forward = forward,
      .node_id = target_id.node_id(),
      .node_class = static_cast<opcua::NodeClass>(node_class),
  };
}

std::optional<opcua::BrowseResult> DecodeSingleBrowseResult(
    const std::vector<char>& payload,
    std::uint32_t expected_encoding_id) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != expected_encoding_id) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  opcua::BrowseResult result;
  std::int32_t reference_count = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status) ||
      !decoder.Decode(result.continuation_point) ||
      !decoder.Decode(reference_count) || reference_count < 0) {
    return std::nullopt;
  }
  result.status_code = static_cast<opcua::StatusCode>(
      static_cast<std::uint16_t>(result_status >> 16));
  result.references.resize(static_cast<std::size_t>(reference_count));
  for (auto& reference : result.references) {
    opcua::ExpandedNodeId target_id;
    opcua::QualifiedName ignored_browse_name;
    opcua::LocalizedText ignored_display_name;
    std::uint32_t ignored_node_class = 0;
    opcua::ExpandedNodeId ignored_type_definition;
    if (!decoder.Decode(reference.reference_type_id) ||
        !decoder.Decode(reference.forward) || !decoder.Decode(target_id) ||
        !decoder.Decode(ignored_browse_name) ||
        !decoder.Decode(ignored_display_name) ||
        !decoder.Decode(ignored_node_class) ||
        !decoder.Decode(ignored_type_definition)) {
      return std::nullopt;
    }
    reference.node_id = target_id.node_id();
  }
  return result;
}

struct DecodedCreateSubscriptionResponse {
  opcua::UInt32 subscription_id = 0;
  double revised_publishing_interval_ms = 0;
  opcua::UInt32 revised_lifetime_count = 0;
  opcua::UInt32 revised_max_keep_alive_count = 0;
};

std::optional<DecodedCreateSubscriptionResponse>
DecodeCreateSubscriptionResponse(const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kCreateSubscriptionResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  DecodedCreateSubscriptionResponse result;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) ||
      !decoder.Decode(result.subscription_id) ||
      !decoder.Decode(result.revised_publishing_interval_ms) ||
      !decoder.Decode(result.revised_lifetime_count) ||
      !decoder.Decode(result.revised_max_keep_alive_count)) {
    return std::nullopt;
  }
  return result;
}

struct DecodedModifySubscriptionResponse {
  std::uint32_t status = 0;
  double revised_publishing_interval_ms = 0;
  opcua::UInt32 revised_lifetime_count = 0;
  opcua::UInt32 revised_max_keep_alive_count = 0;
};

std::optional<DecodedModifySubscriptionResponse>
DecodeModifySubscriptionResponse(const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kModifySubscriptionResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  DecodedModifySubscriptionResponse result;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(result.status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) ||
      !decoder.Decode(result.revised_publishing_interval_ms) ||
      !decoder.Decode(result.revised_lifetime_count) ||
      !decoder.Decode(result.revised_max_keep_alive_count)) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::uint32_t> DecodeSingleSetPublishingModeResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kSetPublishingModeResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

std::optional<std::uint32_t> DecodeSingleDeleteSubscriptionsResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kDeleteSubscriptionsResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

std::optional<opcua::BrowseResult> DecodeSingleBrowseNextResult(
    const std::vector<char>& payload) {
  return DecodeSingleBrowseResult(payload, kBrowseNextResponseEncodingId);
}

std::optional<std::uint32_t> DecodeSingleCallResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kCallResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t method_status = 0;
  std::int32_t input_result_count = 0;
  std::int32_t input_diag_count = 0;
  std::int32_t output_argument_count = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(method_status) || !decoder.Decode(input_result_count) ||
      !decoder.Decode(input_diag_count) ||
      !decoder.Decode(output_argument_count)) {
    return std::nullopt;
  }
  if (input_result_count != 0 || input_diag_count != -1 ||
      output_argument_count != 0) {
    return std::nullopt;
  }
  return method_status;
}

std::optional<opcua::BrowsePathTarget> DecodeSingleTranslateBrowsePathTarget(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kTranslateBrowsePathsResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  std::int32_t target_count = 0;
  opcua::BrowsePathTarget target;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status) || !decoder.Decode(target_count) ||
      target_count != 1 || !decoder.Decode(target.target_id)) {
    return std::nullopt;
  }

  std::uint32_t remaining_path_index = 0;
  if (!decoder.Decode(remaining_path_index)) {
    return std::nullopt;
  }
  target.remaining_path_index = remaining_path_index;
  return target;
}

std::optional<std::uint32_t> DecodeSingleDeleteNodesResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kDeleteNodesResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

std::optional<opcua::AddNodesResult> DecodeSingleAddNodesResponseResult(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kAddNodesResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  opcua::AddNodesResult result;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status) || !decoder.Decode(result.added_node_id)) {
    return std::nullopt;
  }
  result.status_code = opcua::Status::FromFullCode(result_status).code();
  return result;
}

std::optional<std::uint32_t> DecodeSingleDeleteReferencesResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kDeleteReferencesResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }

  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

std::optional<std::uint32_t> DecodeSingleAddReferencesResponseStatus(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kAddReferencesResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte)) {
    return std::nullopt;
  }
  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(result_count) || result_count != 1 ||
      !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

std::optional<std::uint32_t> DecodeSingleResultStatus(
    const std::vector<char>& payload,
    std::uint32_t expected_type_id) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != expected_type_id) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t ignored_status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t result_status = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(ignored_status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(result_status)) {
    return std::nullopt;
  }
  return result_status;
}

struct DecodedHistoryReadRawResponse {
  opcua::Status status{opcua::StatusCode::Bad};
  std::vector<opcua::DataValue> values;
  opcua::ByteString continuation_point;
};

std::optional<DecodedHistoryReadRawResponse> DecodeHistoryReadRawResponse(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kHistoryReadResponseEncodingId) {
    return std::nullopt;
  }

  Decoder decoder{message->second};
  DecodedHistoryReadRawResponse response;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t raw_status = 0;
  DecodedExtensionObject history_data;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) || !decoder.Decode(raw_status) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(raw_status) ||
      !decoder.Decode(response.continuation_point) ||
      !decoder.Decode(history_data) ||
      history_data.type_id != kHistoryDataEncodingId ||
      history_data.encoding != 0x01 || !decoder.Decode(ignored_array) ||
      !decoder.consumed()) {
    return std::nullopt;
  }

  response.status = opcua::Status::FromFullCode(raw_status);
  Decoder history_decoder{history_data.body};
  std::int32_t value_count = 0;
  if (!history_decoder.Decode(value_count) || value_count < 0) {
    return std::nullopt;
  }
  response.values.resize(static_cast<std::size_t>(value_count));
  for (auto& value : response.values) {
    std::uint8_t mask = 0;
    std::uint32_t ignored_status_code = 0;
    std::int64_t ignored_source_timestamp = 0;
    std::int64_t ignored_server_timestamp = 0;
    if (!history_decoder.Decode(mask)) {
      return std::nullopt;
    }
    if ((mask & 0x01) != 0 && !history_decoder.Decode(value.value)) {
      return std::nullopt;
    }
    if ((mask & 0x02) != 0 && !history_decoder.Decode(ignored_status_code)) {
      return std::nullopt;
    }
    if ((mask & 0x04) != 0 &&
        !history_decoder.Decode(ignored_source_timestamp)) {
      return std::nullopt;
    }
    if ((mask & 0x08) != 0 &&
        !history_decoder.Decode(ignored_server_timestamp)) {
      return std::nullopt;
    }
  }
  if (!history_decoder.consumed()) {
    return std::nullopt;
  }
  return response;
}

struct DecodedHistoryReadEventsResponse {
  opcua::Status status{opcua::StatusCode::Bad};
  std::vector<std::vector<opcua::Variant>> events;
};

std::optional<DecodedHistoryReadEventsResponse> DecodeHistoryReadEventsResponse(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kHistoryReadResponseEncodingId) {
    return std::nullopt;
  }

  Decoder decoder{message->second};
  DecodedHistoryReadEventsResponse response;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t raw_status = 0;
  opcua::ByteString continuation_point;
  DecodedExtensionObject history_event;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) || !decoder.Decode(raw_status) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(raw_status) ||
      !decoder.Decode(continuation_point) || !continuation_point.empty() ||
      !decoder.Decode(history_event) ||
      history_event.type_id != kHistoryEventEncodingId ||
      history_event.encoding != 0x01 || !decoder.Decode(ignored_array) ||
      !decoder.consumed()) {
    return std::nullopt;
  }

  response.status = opcua::Status::FromFullCode(raw_status);
  Decoder history_decoder{history_event.body};
  std::int32_t event_count = 0;
  if (!history_decoder.Decode(event_count) || event_count < 0) {
    return std::nullopt;
  }

  response.events.resize(static_cast<std::size_t>(event_count));
  for (auto& event_fields : response.events) {
    std::int32_t field_count = 0;
    if (!history_decoder.Decode(field_count) || field_count < 0) {
      return std::nullopt;
    }
    event_fields.resize(static_cast<std::size_t>(field_count));
    for (auto& field : event_fields) {
      if (!history_decoder.Decode(field)) {
        return std::nullopt;
      }
    }
  }
  if (!history_decoder.consumed()) {
    return std::nullopt;
  }
  return response;
}

struct DecodedCreateMonitoredItemsResponse {
  std::uint32_t status = 0;
  MonitoredItemCreateResult result;
};

std::optional<DecodedCreateMonitoredItemsResponse>
DecodeCreateMonitoredItemsResponse(const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kCreateMonitoredItemsResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  DecodedCreateMonitoredItemsResponse response;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t item_status = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(response.status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(item_status) ||
      !decoder.Decode(response.result.monitored_item_id) ||
      !decoder.Decode(response.result.revised_sampling_interval_ms) ||
      !decoder.Decode(response.result.revised_queue_size)) {
    return std::nullopt;
  }
  DecodedExtensionObject ignored_filter_result;
  if (!decoder.Decode(ignored_filter_result) ||
      !decoder.Decode(ignored_array)) {
    return std::nullopt;
  }
  response.result.status = opcua::Status::FromFullCode(item_status);
  return response;
}

struct DecodedModifyMonitoredItemsResponse {
  std::uint32_t status = 0;
  MonitoredItemModifyResult result;
};

std::optional<DecodedModifyMonitoredItemsResponse>
DecodeModifyMonitoredItemsResponse(const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kModifyMonitoredItemsResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  DecodedModifyMonitoredItemsResponse response;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  std::uint32_t item_status = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(response.status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count != 1 || !decoder.Decode(item_status) ||
      !decoder.Decode(response.result.revised_sampling_interval_ms) ||
      !decoder.Decode(response.result.revised_queue_size)) {
    return std::nullopt;
  }
  DecodedExtensionObject ignored_filter_result;
  if (!decoder.Decode(ignored_filter_result) ||
      !decoder.Decode(ignored_array)) {
    return std::nullopt;
  }
  response.result.status = opcua::Status::FromFullCode(item_status);
  return response;
}

struct DecodedPublishResponse {
  std::uint32_t status = 0;
  opcua::UInt32 subscription_id = 0;
  std::vector<opcua::UInt32> available_sequence_numbers;
  bool more_notifications = false;
  opcua::UInt32 sequence_number = 0;
  bool has_data_change = false;
  double data_change_value = 0;
  bool has_event = false;
  opcua::UInt32 event_client_handle = 0;
  std::vector<opcua::Variant> event_fields;
  std::vector<std::uint32_t> acknowledgement_results;
};

std::optional<DecodedPublishResponse> DecodePublishResponse(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kPublishResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  DecodedPublishResponse response;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t available_count = 0;
  std::int64_t ignored_publish_time = 0;
  std::int32_t notification_count = 0;
  DecodedExtensionObject notification_data;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(response.status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) ||
      !decoder.Decode(response.subscription_id) ||
      !decoder.Decode(available_count) || available_count < 0) {
    return std::nullopt;
  }
  response.available_sequence_numbers.resize(
      static_cast<std::size_t>(available_count));
  for (auto& sequence_number : response.available_sequence_numbers) {
    if (!decoder.Decode(sequence_number)) {
      return std::nullopt;
    }
  }
  if (!decoder.Decode(response.more_notifications) ||
      !decoder.Decode(response.sequence_number) ||
      !decoder.Decode(ignored_publish_time) ||
      !decoder.Decode(notification_count) || notification_count < 0) {
    return std::nullopt;
  }
  if (notification_count == 1) {
    if (!decoder.Decode(notification_data) ||
        notification_data.encoding != 0x01) {
      return std::nullopt;
    }
    Decoder notification_decoder{notification_data.body};
    if (notification_data.type_id == 811) {
      std::int32_t item_count = 0;
      opcua::UInt32 client_handle = 0;
      std::uint8_t value_mask = 0;
      opcua::Variant value;
      std::uint32_t ignored_status_code = 0;
      std::int64_t ignored_source_timestamp = 0;
      std::int64_t ignored_server_timestamp = 0;
      if (!notification_decoder.Decode(item_count) || item_count != 1 ||
          !notification_decoder.Decode(client_handle) ||
          !notification_decoder.Decode(value_mask) ||
          (value_mask & 0x01) == 0 || !notification_decoder.Decode(value) ||
          value.type() != opcua::Variant::DOUBLE) {
        return std::nullopt;
      }
      if ((value_mask & 0x02) != 0 &&
          !notification_decoder.Decode(ignored_status_code)) {
        return std::nullopt;
      }
      if ((value_mask & 0x04) != 0 &&
          !notification_decoder.Decode(ignored_source_timestamp)) {
        return std::nullopt;
      }
      if ((value_mask & 0x08) != 0 &&
          !notification_decoder.Decode(ignored_server_timestamp)) {
        return std::nullopt;
      }
      if (!notification_decoder.consumed()) {
        return std::nullopt;
      }
      response.has_data_change = true;
      response.data_change_value = value.get<opcua::Double>();
    } else if (notification_data.type_id == 916) {
      std::int32_t event_count = 0;
      DecodedExtensionObject event_field_list;
      if (!notification_decoder.Decode(event_count) || event_count != 1 ||
          !notification_decoder.Decode(event_field_list) ||
          event_field_list.type_id != 919 ||
          event_field_list.encoding != 0x01) {
        return std::nullopt;
      }
      Decoder event_decoder{event_field_list.body};
      std::int32_t field_count = 0;
      if (!event_decoder.Decode(response.event_client_handle) ||
          !event_decoder.Decode(field_count) || field_count < 0) {
        return std::nullopt;
      }
      response.event_fields.resize(static_cast<std::size_t>(field_count));
      for (auto& field : response.event_fields) {
        if (!event_decoder.Decode(field)) {
          return std::nullopt;
        }
      }
      if (!event_decoder.consumed() || !notification_decoder.consumed()) {
        return std::nullopt;
      }
      response.has_event = true;
    } else {
      return std::nullopt;
    }
  } else if (notification_count != 0) {
    return std::nullopt;
  }
  std::int32_t ack_count = 0;
  if (!decoder.Decode(ack_count) || ack_count < 0) {
    return std::nullopt;
  }
  response.acknowledgement_results.resize(static_cast<std::size_t>(ack_count));
  for (auto& result : response.acknowledgement_results) {
    if (!decoder.Decode(result)) {
      return std::nullopt;
    }
  }
  if (!decoder.Decode(ignored_array)) {
    return std::nullopt;
  }
  return response;
}

struct DecodedRepublishResponse {
  std::uint32_t status = 0;
  opcua::UInt32 sequence_number = 0;
  double data_change_value = 0;
};

std::optional<DecodedRepublishResponse> DecodeRepublishResponse(
    const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() || message->first != kRepublishResponseEncodingId) {
    return std::nullopt;
  }
  Decoder decoder{message->second};
  DecodedRepublishResponse response;
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int64_t ignored_publish_time = 0;
  std::int32_t notification_count = 0;
  DecodedExtensionObject notification_data;
  std::int32_t item_count = 0;
  opcua::UInt32 client_handle = 0;
  std::uint8_t value_mask = 0;
  opcua::Variant value;
  std::uint32_t ignored_status_code = 0;
  std::int64_t ignored_source_timestamp = 0;
  std::int64_t ignored_server_timestamp = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) ||
      !decoder.Decode(response.status) || !decoder.Decode(ignored_byte) ||
      !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) ||
      !decoder.Decode(response.sequence_number) ||
      !decoder.Decode(ignored_publish_time) ||
      !decoder.Decode(notification_count) || notification_count != 1 ||
      !decoder.Decode(notification_data) || notification_data.type_id != 811 ||
      notification_data.encoding != 0x01) {
    return std::nullopt;
  }
  Decoder notification_decoder{notification_data.body};
  if (!notification_decoder.Decode(item_count) || item_count != 1 ||
      !notification_decoder.Decode(client_handle) ||
      !notification_decoder.Decode(value_mask) || (value_mask & 0x01) == 0 ||
      !notification_decoder.Decode(value) ||
      value.type() != opcua::Variant::DOUBLE) {
    return std::nullopt;
  }
  if ((value_mask & 0x02) != 0 &&
      !notification_decoder.Decode(ignored_status_code)) {
    return std::nullopt;
  }
  if ((value_mask & 0x04) != 0 &&
      !notification_decoder.Decode(ignored_source_timestamp)) {
    return std::nullopt;
  }
  if ((value_mask & 0x08) != 0 &&
      !notification_decoder.Decode(ignored_server_timestamp)) {
    return std::nullopt;
  }
  if (!notification_decoder.consumed()) {
    return std::nullopt;
  }
  response.data_change_value = value.get<opcua::Double>();
  return response;
}

std::optional<std::vector<std::uint32_t>>
DecodeTransferSubscriptionsResponseResults(const std::vector<char>& payload) {
  Decoder message_decoder{payload};
  const auto message = ReadMessage(message_decoder);
  if (!message.has_value() ||
      message->first != kTransferSubscriptionsResponseEncodingId) {
    return std::nullopt;
  }

  Decoder decoder{message->second};
  std::int64_t ignored_timestamp = 0;
  std::uint32_t ignored_request_handle = 0;
  std::uint32_t status = 0;
  std::uint8_t ignored_byte = 0;
  std::int32_t ignored_array = 0;
  opcua::NodeId ignored_header_extension;
  std::int32_t result_count = 0;
  if (!decoder.Decode(ignored_timestamp) ||
      !decoder.Decode(ignored_request_handle) || !decoder.Decode(status) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(ignored_array) ||
      !decoder.Decode(ignored_header_extension) ||
      !decoder.Decode(ignored_byte) || !decoder.Decode(result_count) ||
      result_count < 0) {
    return std::nullopt;
  }
  if (opcua::Status::FromFullCode(status).code() != opcua::StatusCode::Good) {
    return std::nullopt;
  }

  std::vector<std::uint32_t> results(static_cast<size_t>(result_count));
  for (auto& result : results) {
    if (!decoder.Decode(result)) {
      return std::nullopt;
    }
  }

  std::int32_t diagnostics = 0;
  if (!decoder.Decode(diagnostics) || diagnostics != -1 ||
      !decoder.consumed()) {
    return std::nullopt;
  }
  return results;
}

class TestMonitoredItemService : public scada::MonitoredItemService {
 public:
  std::shared_ptr<scada::MonitoredItem> CreateMonitoredItem(
      const opcua::ReadValueId& value_id,
      const opcua::MonitoringParameters& parameters) {
    created_value_ids.push_back(value_id);
    created_parameters.push_back(parameters);
    auto item = std::make_shared<opcua::TestMonitoredItem>();
    items.push_back(item);
    return item;
  }

  opcua::StatusOr<std::unique_ptr<MonitoredItemSubscription>>
  CreateSubscription(opcua::ServiceContext /*context*/,
                     MonitoredItemSubscriptionOptions options) override {
    return scada::MakeItemFactorySubscription(
        [this](const opcua::ReadValueId& value_id,
               const opcua::MonitoringParameters& parameters) {
          return CreateMonitoredItem(value_id, parameters);
        },
        options);
  }

  std::vector<opcua::ReadValueId> created_value_ids;
  std::vector<opcua::MonitoringParameters> created_parameters;
  std::vector<std::shared_ptr<opcua::TestMonitoredItem>> items;
};

class ServiceDispatcherTest : public ::testing::Test {
 protected:
  // The monitored-item subscription pump parks its read loop on an asio
  // steady_timer; resuming it after a notification is pushed needs the timer
  // service to settle, which a single `Drain` does not guarantee. Spin the
  // executor (mirroring `ClientTest::DrainUntil`) so async notification
  // delivery completes before assertions.
  void DrainPump(opcua::TestExecutor& executor) {
    for (int i = 0; i < 200; ++i) {
      Drain(executor);
      std::this_thread::yield();
    }
  }

  void RunPeer(const std::shared_ptr<StreamPeerState>& peer,
               ServiceDispatcher& dispatcher) {
    opcua::WaitAwaitable(
        executor_,
        TcpConnection{
            {.transport = transport::any_transport{ScriptedStreamTransport{
                 any_executor_, peer}},
             .limits = server_limits_,
             .on_secure_frame = [&dispatcher](std::vector<char> payload,
                                              SecureFrameContext)
                 -> opcua::Awaitable<std::optional<std::vector<char>>> {
               co_return co_await dispatcher.HandlePayload(std::move(payload));
             }}}
            .Run());
  }

  opcua::base::Time now_ = ParseTime("2026-04-21 11:00:00");
  const opcua::NodeId expected_user_id_ = NumericNode(700, 5);
  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
  const TransportLimits server_limits_{
      .protocol_version = 0,
      .receive_buffer_size = 8192,
      .send_buffer_size = 4096,
      .max_message_size = 0,
      .max_chunk_count = 0,
  };
  StrictMock<opcua::MockAttributeService> attribute_service_;
  StrictMock<opcua::MockViewService> view_service_;
  StrictMock<opcua::MockHistoryService> history_service_;
  StrictMock<opcua::MockMethodService> method_service_;
  StrictMock<opcua::MockNodeManagementService> node_management_service_;
  TestMonitoredItemService monitored_item_service_;
  ServerSessionManager session_manager_{{
      .authenticator = opcua::MakeCoroutineAuthenticator(
          [this](opcua::LocalizedText user_name, opcua::LocalizedText password)
              -> opcua::Awaitable<
                  opcua::StatusOr<opcua::AuthenticationResult>> {
            EXPECT_EQ(user_name, opcua::LocalizedText{u"operator"});
            EXPECT_EQ(password, opcua::LocalizedText{u"secret"});
            co_return opcua::AuthenticationResult{.user_id = expected_user_id_,
                                                  .multi_sessions = true};
          }),
      .now = [this] { return now_; },
  }};
  ConnectionState connection_;
  Runtime runtime_{{
      .executor = any_executor_,
      .session_manager = session_manager_,
      .monitored_item_service = monitored_item_service_,
      .attribute_service = attribute_service_,
      .view_service = view_service_,
      .history_service = history_service_,
      .method_service = method_service_,
      .node_management_service = node_management_service_,
      .now = [this] { return now_; },
  }};
};

TEST_F(ServiceDispatcherTest,
       ActivateSessionWithUnknownAuthenticationTokenReturnsNoPayload) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     1, NumericNode(999, 3), "operator", "secret")));
  EXPECT_FALSE(activated.has_value());
}

TEST_F(ServiceDispatcherTest,
       CloseSessionWithUnknownAuthenticationTokenReturnsLoggedOff) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto closed = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(
                     EncodeCloseSessionRequestBody(1, NumericNode(999, 3))));
  ASSERT_TRUE(closed.has_value());
  const auto close_status =
      DecodeResponseStatus(*closed, kCloseSessionResponseEncodingId);
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(
      *close_status,
      opcua::Status(opcua::StatusCode::Bad_SessionIsLoggedOff).full_code());
}

TEST_F(ServiceDispatcherTest, HandlesActivateAndCloseSessionOverPayload) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());
  const auto activate_status =
      DecodeResponseStatus(*activated, kActivateSessionResponseEncodingId);
  ASSERT_TRUE(activate_status.has_value());
  EXPECT_EQ(*activate_status, 0u);
  ASSERT_TRUE(connection_.authentication_token.has_value());
  EXPECT_EQ(*connection_.authentication_token, session->authentication_token);

  const auto closed = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCloseSessionRequestBody(
                     3, session->authentication_token)));
  ASSERT_TRUE(closed.has_value());
  const auto close_status =
      DecodeResponseStatus(*closed, kCloseSessionResponseEncodingId);
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(*close_status, 0u);
  EXPECT_FALSE(connection_.authentication_token.has_value());
}

TEST_F(ServiceDispatcherTest, HandlesReadAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::ReadValueId read_value_id{
      .node_id = NumericNode(12),
      .attribute_id = opcua::AttributeId::Value,
  };
  EXPECT_CALL(attribute_service_, Read(_, _))
      .WillOnce(Invoke(
          [&](opcua::ServiceContext context,
              std::shared_ptr<const std::vector<opcua::ReadValueId>> inputs)
              -> opcua::Awaitable<
                  opcua::StatusOr<std::vector<opcua::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_EQ(inputs->size(), 1u);
            if (inputs->size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ((*inputs)[0], read_value_id);
            co_return std::vector{
                opcua::DataValue{opcua::Variant{42.5}, {}, now_, now_}};
          }));

  const auto read = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeReadRequestBody(
                     3, session->authentication_token, read_value_id)));
  ASSERT_TRUE(read.has_value());
  const auto value = DecodeSingleDoubleReadResponse(*read);
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(*value, 42.5);
}

TEST_F(ServiceDispatcherTest, BootstrapsAndReadsEndToEndOverTcpConnection) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  auto peer = std::make_shared<StreamPeerState>();
  const auto hello =
      EncodeHelloMessage({.protocol_version = 0,
                          .receive_buffer_size = 16384,
                          .send_buffer_size = 2048,
                          .max_message_size = 0,
                          .max_chunk_count = 0,
                          .endpoint_url = "opc.tcp://localhost:4840"});
  const auto open = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureOpen,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 0,
       .asymmetric_security_header =
           AsymmetricSecurityHeader{
               .security_policy_uri = std::string{kSecurityPolicyNone},
               .sender_certificate = {},
               .receiver_certificate_thumbprint = {},
           },
       .sequence_header = {.sequence_number = 1, .request_id = 1},
       .body = EncodeOpenRequestBody(1)});
  const auto create = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureMessage,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header = SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = 2, .request_id = 10},
       .body = EncodeCreateSessionRequestBody(10, 45000)});
  const auto activate = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureMessage,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header = SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = 3, .request_id = 11},
       .body = EncodeUserNameActivateRequestBody(11, NumericNode(1, 3),
                                                 "operator", "secret")});
  const auto read = EncodeSecureConversationMessage(
      {.frame_header = {.message_type = MessageType::SecureMessage,
                        .chunk_type = 'F',
                        .message_size = 0},
       .secure_channel_id = 1,
       .symmetric_security_header = SymmetricSecurityHeader{.token_id = 1},
       .sequence_header = {.sequence_number = 4, .request_id = 12},
       .body =
           EncodeReadRequestBody(12, NumericNode(1, 3),
                                 {.node_id = NumericNode(99),
                                  .attribute_id = opcua::AttributeId::Value})});

  peer->incoming.push_back(AsString(hello));
  peer->incoming.push_back(AsString(open));
  peer->incoming.push_back(AsString(create));
  peer->incoming.push_back(AsString(activate));
  peer->incoming.push_back(AsString(read));

  EXPECT_CALL(attribute_service_, Read(_, _))
      .WillOnce(Invoke(
          [&](opcua::ServiceContext context,
              std::shared_ptr<const std::vector<opcua::ReadValueId>> inputs)
              -> opcua::Awaitable<
                  opcua::StatusOr<std::vector<opcua::DataValue>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_EQ((*inputs)[0].node_id, NumericNode(99));
            co_return std::vector{
                opcua::DataValue{opcua::Variant{12.5}, {}, now_, now_}};
          }));

  RunPeer(peer, dispatcher);

  ASSERT_GE(peer->writes.size(), 5u);
  const auto read_response_frame = DecodeSecureConversationMessage(
      std::vector<char>{peer->writes[4].begin(), peer->writes[4].end()});
  ASSERT_TRUE(read_response_frame.has_value());
  const auto value = DecodeSingleDoubleReadResponse(read_response_frame->body);
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(*value, 12.5);
}

TEST_F(ServiceDispatcherTest, HandlesWriteAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  EXPECT_CALL(attribute_service_, Write(_, _))
      .WillOnce(Invoke(
          [this](opcua::ServiceContext context,
                 std::shared_ptr<const std::vector<opcua::WriteValue>> inputs)
              -> opcua::Awaitable<
                  opcua::StatusOr<std::vector<opcua::StatusCode>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_EQ(inputs->size(), 1u);
            if (inputs->size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ((*inputs)[0].node_id, NumericNode(12));
            EXPECT_EQ((*inputs)[0].attribute_id, opcua::AttributeId::Value);
            EXPECT_DOUBLE_EQ((*inputs)[0].value.get<opcua::Double>(), 42.0);
            co_return std::vector{opcua::StatusCode::Good};
          }));

  const auto written = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeWriteRequestBody(
                     3, session->authentication_token,
                     {.node_id = NumericNode(12),
                      .attribute_id = opcua::AttributeId::Value,
                      .value = opcua::Double{42.0}})));
  ASSERT_TRUE(written.has_value());
  const auto status = DecodeSingleWriteResponseStatus(*written);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesBrowseAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::BrowseDescription browse_description{
      .node_id = NumericNode(12),
      .direction = opcua::BrowseDirection::Forward,
      .reference_type_id = NumericNode(45),
      .include_subtypes = false,
  };
  EXPECT_CALL(view_service_, Browse(_, _))
      .WillOnce(
          Invoke([&](opcua::ServiceContext context,
                     std::vector<opcua::BrowseDescription> inputs)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::BrowseResult>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(inputs[0], browse_description);
            co_return std::vector{opcua::BrowseResult{
                .status_code = opcua::StatusCode::Good,
                .references = {{.reference_type_id = NumericNode(46),
                                .forward = true,
                                .node_id = NumericNode(99)}}}};
          }));

  const auto browsed = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeBrowseRequestBody(
                     3, session->authentication_token, browse_description)));
  ASSERT_TRUE(browsed.has_value());
  const auto reference = DecodeSingleBrowseReference(*browsed);
  ASSERT_TRUE(reference.has_value());
  EXPECT_EQ(reference->reference_type_id, NumericNode(46));
  EXPECT_TRUE(reference->forward);
  EXPECT_EQ(reference->node_id, NumericNode(99));
}

TEST_F(ServiceDispatcherTest, HandlesBrowseNextAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::BrowseDescription browse_description{
      .node_id = NumericNode(12),
      .direction = opcua::BrowseDirection::Forward,
      .reference_type_id = NumericNode(45),
      .include_subtypes = false,
  };
  EXPECT_CALL(view_service_, Browse(_, _))
      .WillOnce(
          Invoke([&](opcua::ServiceContext context,
                     std::vector<opcua::BrowseDescription> inputs)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::BrowseResult>>> {
            EXPECT_EQ(context.user_id(), expected_user_id_);
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(inputs[0], browse_description);
            co_return std::vector{opcua::BrowseResult{
                .status_code = opcua::StatusCode::Good,
                .references = {{.reference_type_id = NumericNode(46),
                                .forward = true,
                                .node_id = NumericNode(99)},
                               {.reference_type_id = NumericNode(47),
                                .forward = true,
                                .node_id = NumericNode(100)}}}};
          }));

  const auto browsed = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeBrowseRequestBody(
                     3, session->authentication_token, browse_description, 1)));
  ASSERT_TRUE(browsed.has_value());

  const auto browse_page =
      DecodeSingleBrowseResult(*browsed, kBrowseResponseEncodingId);
  ASSERT_TRUE(browse_page.has_value());
  const auto paged_reference = DecodeSingleBrowseReference(*browsed);
  ASSERT_TRUE(paged_reference.has_value());
  EXPECT_EQ(paged_reference->reference_type_id, NumericNode(46));
  EXPECT_FALSE(browse_page->continuation_point.empty());

  const auto resumed = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeBrowseNextRequestBody(
                     4, session->authentication_token, false,
                     {browse_page->continuation_point})));
  ASSERT_TRUE(resumed.has_value());
  const auto result = DecodeSingleBrowseNextResult(*resumed);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status_code, opcua::StatusCode::Good);
  ASSERT_EQ(result->references.size(), 1u);
  EXPECT_EQ(result->references[0].reference_type_id, NumericNode(47));
  EXPECT_TRUE(result->references[0].forward);
  EXPECT_EQ(result->references[0].node_id, NumericNode(100));
  EXPECT_TRUE(result->continuation_point.empty());
}

TEST_F(ServiceDispatcherTest,
       HandlesTranslateBrowsePathsAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::BrowsePath browse_path{
      .node_id = NumericNode(12),
      .relative_path = {{
          .reference_type_id = NumericNode(33),
          .inverse = false,
          .include_subtypes = true,
          .target_name = {"Temperature", 2},
      }},
  };
  EXPECT_CALL(view_service_, TranslateBrowsePaths(_))
      .WillOnce(Invoke(
          [&](std::vector<opcua::BrowsePath> inputs)
              -> opcua::Awaitable<
                  opcua::StatusOr<std::vector<opcua::BrowsePathResult>>> {
            EXPECT_EQ(inputs.size(), 1u);
            if (inputs.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(inputs[0], browse_path);
            co_return std::vector{opcua::BrowsePathResult{
                .status_code = opcua::StatusCode::Good,
                .targets = {
                    {.target_id = opcua::ExpandedNodeId{NumericNode(77)},
                     .remaining_path_index = 0}}}};
          }));

  const auto translated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeTranslateBrowsePathsRequestBody(
                     3, session->authentication_token, browse_path)));
  ASSERT_TRUE(translated.has_value());
  const auto target = DecodeSingleTranslateBrowsePathTarget(*translated);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->target_id.node_id(), NumericNode(77));
  EXPECT_EQ(target->remaining_path_index, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesHistoryReadRawAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto from = now_ - opcua::base::TimeDelta::FromMinutes(15);
  const auto to = now_;
  EXPECT_CALL(history_service_, HistoryReadRaw(_))
      .WillOnce(Invoke([&](opcua::HistoryReadRawDetails details)
                           -> opcua::Awaitable<opcua::HistoryReadRawResult> {
        EXPECT_TRUE(details.node_id == NumericNode(120));
        EXPECT_EQ(details.from, from);
        EXPECT_EQ(details.to, to);
        EXPECT_EQ(details.max_count, 25u);
        EXPECT_TRUE(details.release_continuation_point);
        EXPECT_EQ(details.continuation_point, (opcua::ByteString{4, 5, 6}));
        co_return opcua::HistoryReadRawResult{
            .status = opcua::StatusCode::Good,
            .values = {opcua::DataValue{
                opcua::Variant{42.5},
                {},
                now_,
                now_,
            }},
            .continuation_point = {7, 8, 9},
        };
      }));

  const auto history_read = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeHistoryReadRawRequestBody(
                     3, session->authentication_token,
                     {.node_id = NumericNode(120),
                      .from = from,
                      .to = to,
                      .max_count = 25,
                      .release_continuation_point = true,
                      .continuation_point = {4, 5, 6}})));
  ASSERT_TRUE(history_read.has_value());
  const auto decoded = DecodeHistoryReadRawResponse(*history_read);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(decoded->continuation_point, (opcua::ByteString{7, 8, 9}));
  ASSERT_EQ(decoded->values.size(), 1u);
  ASSERT_EQ(decoded->values[0].value.type(), opcua::Variant::DOUBLE);
  EXPECT_DOUBLE_EQ(decoded->values[0].value.get<opcua::Double>(), 42.5);
}

TEST_F(ServiceDispatcherTest, RejectsHistoryReadRawWithoutActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto history_read = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeHistoryReadRawRequestBody(
                     1, NumericNode(999, 3),
                     {.node_id = NumericNode(120),
                      .from = now_ - opcua::base::TimeDelta::FromMinutes(15),
                      .to = now_,
                      .max_count = 25})));
  ASSERT_TRUE(history_read.has_value());
  const auto decoded = DecodeHistoryReadRawResponse(*history_read);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status.code(), opcua::StatusCode::Bad_SessionIsLoggedOff);
  EXPECT_TRUE(decoded->values.empty());
  EXPECT_TRUE(decoded->continuation_point.empty());
}

TEST_F(ServiceDispatcherTest, HandlesHistoryReadEventsAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto from = now_ - opcua::base::TimeDelta::FromMinutes(30);
  const auto to = now_;
  const opcua::EventFilter filter{
      .of_type = {NumericNode(301)},
      .child_of = {NumericNode(302)},
  };
  EXPECT_CALL(history_service_, HistoryReadEvents(_, _, _, _))
      .WillOnce(Invoke([&](opcua::NodeId node_id, opcua::base::Time actual_from,
                           opcua::base::Time actual_to,
                           opcua::EventFilter actual_filter)
                           -> opcua::Awaitable<opcua::HistoryReadEventsResult> {
        EXPECT_TRUE(node_id == NumericNode(300));
        EXPECT_EQ(actual_from, from);
        EXPECT_EQ(actual_to, to);
        EXPECT_EQ(actual_filter, filter);

        opcua::Event event;
        event.event_id = 55;
        event.event_type_id = opcua::id::SystemEventType;
        event.time = now_;
        event.receive_time = now_;
        event.node_id = NumericNode(303);
        event.message = opcua::LocalizedText{u"alarm"};
        event.severity = 700;
        co_return opcua::HistoryReadEventsResult{
            .status = opcua::StatusCode::Good,
            .events = {std::move(event)},
        };
      }));

  const auto history_read = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeHistoryReadEventsRequestBody(
                     3, session->authentication_token,
                     {.node_id = NumericNode(300),
                      .from = from,
                      .to = to,
                      .filter = filter})));
  ASSERT_TRUE(history_read.has_value());
  const auto decoded = DecodeHistoryReadEventsResponse(*history_read);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status.code(), opcua::StatusCode::Good);
  ASSERT_EQ(decoded->events.size(), 1u);
  ASSERT_EQ(decoded->events[0].size(), 7u);
  EXPECT_EQ(decoded->events[0][0].get<opcua::UInt64>(), 55u);
  EXPECT_EQ(decoded->events[0][1].get<opcua::NodeId>(),
            opcua::NodeId{opcua::id::SystemEventType});
  EXPECT_EQ(decoded->events[0][2].get<opcua::NodeId>(), NumericNode(303));
  EXPECT_EQ(decoded->events[0][3].get<opcua::String>(),
            NumericNode(303).ToString());
  EXPECT_EQ(decoded->events[0][4].get<opcua::DateTime>(), now_);
  EXPECT_EQ(decoded->events[0][5].get<opcua::LocalizedText>(),
            opcua::LocalizedText{u"alarm"});
  EXPECT_EQ(decoded->events[0][6].get<opcua::UInt32>(), 700u);
}

TEST_F(ServiceDispatcherTest, RejectsHistoryReadEventsWithoutActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto history_read = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeHistoryReadEventsRequestBody(
                     1, NumericNode(999, 3),
                     {.node_id = NumericNode(300),
                      .from = now_ - opcua::base::TimeDelta::FromMinutes(30),
                      .to = now_})));
  ASSERT_TRUE(history_read.has_value());
  const auto decoded = DecodeHistoryReadEventsResponse(*history_read);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status.code(), opcua::StatusCode::Bad_SessionIsLoggedOff);
  EXPECT_TRUE(decoded->events.empty());
}

TEST_F(ServiceDispatcherTest, HandlesCreateSubscriptionAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded = DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_NE(decoded->subscription_id, 0u);
  EXPECT_DOUBLE_EQ(decoded->revised_publishing_interval_ms, 100.0);
  EXPECT_EQ(decoded->revised_lifetime_count, 60u);
  EXPECT_EQ(decoded->revised_max_keep_alive_count, 3u);
}

TEST_F(ServiceDispatcherTest, HandlesModifySubscriptionAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto modified = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeModifySubscriptionRequestBody(
                     4, session->authentication_token,
                     decoded_subscription->subscription_id,
                     {.publishing_interval_ms = 250,
                      .lifetime_count = 80,
                      .max_keep_alive_count = 5,
                      .publishing_enabled = true})));
  ASSERT_TRUE(modified.has_value());
  const auto decoded_modified = DecodeModifySubscriptionResponse(*modified);
  ASSERT_TRUE(decoded_modified.has_value());
  EXPECT_EQ(decoded_modified->status, 0u);
  EXPECT_DOUBLE_EQ(decoded_modified->revised_publishing_interval_ms, 250.0);
  EXPECT_EQ(decoded_modified->revised_lifetime_count, 80u);
  EXPECT_EQ(decoded_modified->revised_max_keep_alive_count, 5u);
}

TEST_F(ServiceDispatcherTest, HandlesSetPublishingModeAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto set_mode = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeSetPublishingModeRequestBody(
                     4, session->authentication_token, false,
                     {decoded_subscription->subscription_id})));
  ASSERT_TRUE(set_mode.has_value());
  const auto status = DecodeSingleSetPublishingModeResponseStatus(*set_mode);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesDeleteSubscriptionsAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded = DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded.has_value());

  const auto deleted = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeDeleteSubscriptionsRequestBody(
          4, session->authentication_token, {decoded->subscription_id})));
  ASSERT_TRUE(deleted.has_value());
  const auto status = DecodeSingleDeleteSubscriptionsResponseStatus(*deleted);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest,
       HandlesCreateMonitoredItemsAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  const auto decoded = DecodeCreateMonitoredItemsResponse(*created_items);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, 0u);
  EXPECT_EQ(decoded->result.status.code(), opcua::StatusCode::Good);
  EXPECT_NE(decoded->result.monitored_item_id, 0u);
  ASSERT_EQ(monitored_item_service_.created_value_ids.size(), 1u);
  EXPECT_EQ(monitored_item_service_.created_value_ids[0],
            (opcua::ReadValueId{.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value}));
}

TEST_F(ServiceDispatcherTest, DecodesDataChangeFilterForCreateMonitoredItems) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {
                .client_handle = 44,
                .sampling_interval_ms = 0,
                .filter = MonitoringFilter{DataChangeFilter{
                    .trigger = DataChangeTrigger::StatusValueTimestamp,
                    .deadband_type = DeadbandType::Absolute,
                    .deadband_value = 1.5}},
                .queue_size = 1,
                .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  const auto decoded_items = DecodeCreateMonitoredItemsResponse(*created_items);
  ASSERT_TRUE(decoded_items.has_value());
  EXPECT_EQ(decoded_items->result.status.code(), opcua::StatusCode::Good);

  ASSERT_EQ(monitored_item_service_.created_parameters.size(), 1u);
  const auto& created_filter =
      monitored_item_service_.created_parameters[0].filter;
  ASSERT_TRUE(created_filter.has_value());
  const auto* filter = std::get_if<opcua::DataChangeFilter>(&*created_filter);
  ASSERT_NE(filter, nullptr);
  EXPECT_DOUBLE_EQ(filter->deadband_value, 1.5);
}

TEST_F(ServiceDispatcherTest,
       HandlesModifyMonitoredItemsAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  const auto decoded_created =
      DecodeCreateMonitoredItemsResponse(*created_items);
  ASSERT_TRUE(decoded_created.has_value());

  const auto modified = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeModifyMonitoredItemsRequestBody(
          5, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.monitored_item_id = decoded_created->result.monitored_item_id,
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 250,
                                     .queue_size = 3,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(modified.has_value());
  const auto decoded_modified = DecodeModifyMonitoredItemsResponse(*modified);
  ASSERT_TRUE(decoded_modified.has_value());
  EXPECT_EQ(decoded_modified->status, 0u);
  EXPECT_EQ(decoded_modified->result.status.code(), opcua::StatusCode::Good);
  EXPECT_DOUBLE_EQ(decoded_modified->result.revised_sampling_interval_ms,
                   250.0);
  EXPECT_EQ(decoded_modified->result.revised_queue_size, 3u);
  ASSERT_EQ(monitored_item_service_.created_parameters.size(), 2u);
  EXPECT_EQ(monitored_item_service_.created_parameters[1].sampling_interval_ms,
            250);
  EXPECT_EQ(monitored_item_service_.created_parameters[1].queue_size, 3u);
}

TEST_F(ServiceDispatcherTest, HandlesPublishAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  ASSERT_EQ(monitored_item_service_.items.size(), 1u);
  // The backing monitored item is wired up asynchronously on the executor, so
  // settle the subscription pump's read loop before notifying.
  DrainPump(executor_);
  monitored_item_service_.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{12.5}, {}, now_, now_});
  // The notification flows through the subscription pump's async read loop
  // (which parks on an asio steady_timer), so spin the executor until the value
  // reaches the queue before publishing.
  DrainPump(executor_);
  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(100);

  const auto published = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodePublishRequestBody(
                     5, session->authentication_token)));
  ASSERT_TRUE(published.has_value());
  const auto decoded = DecodePublishResponse(*published);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, 0u);
  EXPECT_EQ(decoded->subscription_id, decoded_subscription->subscription_id);
  EXPECT_THAT(decoded->available_sequence_numbers,
              ElementsAre(decoded->sequence_number));
  EXPECT_FALSE(decoded->more_notifications);
  EXPECT_TRUE(decoded->has_data_change);
  EXPECT_DOUBLE_EQ(decoded->data_change_value, 12.5);
}

TEST_F(ServiceDispatcherTest,
       HandlesEventPublishWithEventFilterAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(2253),
                                .attribute_id =
                                    opcua::AttributeId::EventNotifier},
            .requested_parameters = {
                .client_handle = 55,
                .sampling_interval_ms = 0,
                .filter = MonitoringFilter{BuildEventFilter(
                    std::vector<std::vector<std::string>>{
                        {"Message"}, {"Severity"}, {"EventId"}})},
                .queue_size = 1,
                .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  const auto decoded_items = DecodeCreateMonitoredItemsResponse(*created_items);
  ASSERT_TRUE(decoded_items.has_value());
  EXPECT_EQ(decoded_items->result.status.code(), opcua::StatusCode::Good);

  opcua::Event event;
  event.event_id = 77;
  event.event_type_id = NumericNode(2041);
  event.node_id = NumericNode(3001);
  event.time = now_;
  event.receive_time = now_;
  event.message = opcua::LocalizedText{u"custom alarm"};
  event.severity = 600;
  ASSERT_EQ(monitored_item_service_.items.size(), 1u);
  monitored_item_service_.items[0]->NotifyEvent(event);
  // The notification flows through the subscription pump's async read loop
  // (which parks on an asio steady_timer), so spin the executor until the event
  // reaches the queue before publishing.
  DrainPump(executor_);
  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(100);

  const auto published = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodePublishRequestBody(
                     5, session->authentication_token)));
  ASSERT_TRUE(published.has_value());
  const auto decoded = DecodePublishResponse(*published);
  ASSERT_TRUE(decoded.has_value());
  // The event is projected onto the EventFilter select clauses
  // ({Message},{Severity},{EventId}) at the data source and carried across the
  // notification boundary as a standard EventFieldList, so the real field
  // values flow end-to-end to the right monitored item (client_handle).
  EXPECT_TRUE(decoded->has_event);
  EXPECT_EQ(decoded->event_client_handle, 55u);
  ASSERT_EQ(decoded->event_fields.size(), 3u);
  EXPECT_EQ(decoded->event_fields[0].get<opcua::LocalizedText>(),
            opcua::LocalizedText{u"custom alarm"});
  EXPECT_EQ(decoded->event_fields[1].get<opcua::UInt32>(), 600u);
  EXPECT_EQ(decoded->event_fields[2].get<opcua::UInt64>(), 77u);
}

TEST_F(ServiceDispatcherTest,
       HandlesPublishAcknowledgementsAndKeepAliveAfterDisablingPublishing) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 2,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  ASSERT_EQ(monitored_item_service_.items.size(), 1u);
  monitored_item_service_.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{12.5}, {}, now_, now_});
  // The notification flows through the subscription pump's async read loop
  // (which parks on an asio steady_timer), so spin the executor until the value
  // reaches the queue before publishing.
  DrainPump(executor_);

  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(100);
  const auto published = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodePublishRequestBody(
                     5, session->authentication_token)));
  ASSERT_TRUE(published.has_value());
  const auto decoded_publish = DecodePublishResponse(*published);
  ASSERT_TRUE(decoded_publish.has_value());
  EXPECT_TRUE(decoded_publish->has_data_change);
  EXPECT_EQ(decoded_publish->acknowledgement_results.size(), 0u);

  const auto set_mode = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeSetPublishingModeRequestBody(
                     6, session->authentication_token, false,
                     {decoded_subscription->subscription_id})));
  ASSERT_TRUE(set_mode.has_value());
  const auto set_mode_status =
      DecodeSingleSetPublishingModeResponseStatus(*set_mode);
  ASSERT_TRUE(set_mode_status.has_value());
  EXPECT_EQ(*set_mode_status, 0u);

  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(300);
  const auto keep_alive = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodePublishRequestBody(
                     7, session->authentication_token,
                     {{.subscription_id = decoded_subscription->subscription_id,
                       .sequence_number = decoded_publish->sequence_number}})));
  ASSERT_TRUE(keep_alive.has_value());
  const auto decoded_keep_alive = DecodePublishResponse(*keep_alive);
  ASSERT_TRUE(decoded_keep_alive.has_value());
  EXPECT_FALSE(decoded_keep_alive->has_data_change);
  EXPECT_EQ(decoded_keep_alive->subscription_id,
            decoded_subscription->subscription_id);
  EXPECT_EQ(decoded_keep_alive->sequence_number, 2u);
  EXPECT_THAT(decoded_keep_alive->acknowledgement_results, ElementsAre(0u));
}

TEST_F(ServiceDispatcherTest, HandlesRepublishAfterPublish) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  ASSERT_EQ(monitored_item_service_.items.size(), 1u);
  // The backing monitored item is wired up asynchronously on the executor, so
  // settle the subscription pump's read loop before notifying.
  DrainPump(executor_);
  monitored_item_service_.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{12.5}, {}, now_, now_});
  // The notification flows through the subscription pump's async read loop
  // (which parks on an asio steady_timer), so spin the executor until the value
  // reaches the queue before publishing.
  DrainPump(executor_);
  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(100);

  const auto published = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodePublishRequestBody(
                     5, session->authentication_token)));
  ASSERT_TRUE(published.has_value());
  const auto decoded_publish = DecodePublishResponse(*published);
  ASSERT_TRUE(decoded_publish.has_value());

  const auto republished = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeRepublishRequestBody(
                     6, session->authentication_token,
                     decoded_subscription->subscription_id,
                     decoded_publish->sequence_number)));
  ASSERT_TRUE(republished.has_value());
  const auto decoded_republish = DecodeRepublishResponse(*republished);
  ASSERT_TRUE(decoded_republish.has_value());
  EXPECT_EQ(decoded_republish->status, 0u);
  EXPECT_EQ(decoded_republish->sequence_number,
            decoded_publish->sequence_number);
  EXPECT_DOUBLE_EQ(decoded_republish->data_change_value, 12.5);
}

TEST_F(ServiceDispatcherTest,
       HandlesTransferSubscriptionsAcrossActivatedSessions) {
  ServiceDispatcher source_dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_, source_dispatcher.HandlePayload(
                     EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto source_session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(source_session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_,
      source_dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
          2, source_session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_,
      source_dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
          3, source_session->authentication_token,
          {.publishing_interval_ms = 100,
           .lifetime_count = 60,
           .max_keep_alive_count = 3,
           .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      source_dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, source_session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  ASSERT_EQ(monitored_item_service_.items.size(), 1u);
  monitored_item_service_.items[0]->NotifyDataChange(
      opcua::DataValue{opcua::Variant{77.0}, {}, now_, now_});

  ConnectionState target_connection;
  ServiceDispatcher target_dispatcher{
      {.runtime = runtime_, .connection = target_connection}};

  const auto target_created = opcua::WaitAwaitable(
      executor_, target_dispatcher.HandlePayload(
                     EncodeCreateSessionRequestBody(5, 45000)));
  ASSERT_TRUE(target_created.has_value());
  const auto target_session = DecodeCreateSessionResponse(*target_created);
  ASSERT_TRUE(target_session.has_value());

  const auto target_activated = opcua::WaitAwaitable(
      executor_,
      target_dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
          6, target_session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(target_activated.has_value());

  const auto transferred = opcua::WaitAwaitable(
      executor_,
      target_dispatcher.HandlePayload(EncodeTransferSubscriptionsRequestBody(
          7, target_session->authentication_token,
          {decoded_subscription->subscription_id}, true)));
  ASSERT_TRUE(transferred.has_value());
  const auto transfer_results =
      DecodeTransferSubscriptionsResponseResults(*transferred);
  ASSERT_TRUE(transfer_results.has_value());
  EXPECT_THAT(*transfer_results, ElementsAre(0u));

  now_ = now_ + opcua::base::TimeDelta::FromMilliseconds(100);
  const auto target_publish = opcua::WaitAwaitable(
      executor_, target_dispatcher.HandlePayload(EncodePublishRequestBody(
                     8, target_session->authentication_token)));
  ASSERT_TRUE(target_publish.has_value());
  const auto target_decoded = DecodePublishResponse(*target_publish);
  ASSERT_TRUE(target_decoded.has_value());
  EXPECT_EQ(target_decoded->subscription_id,
            decoded_subscription->subscription_id);
  EXPECT_DOUBLE_EQ(target_decoded->data_change_value, 77.0);
}

TEST_F(ServiceDispatcherTest, HandlesSetMonitoringModeAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  const auto decoded_items = DecodeCreateMonitoredItemsResponse(*created_items);
  ASSERT_TRUE(decoded_items.has_value());

  const auto set_mode = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeSetMonitoringModeRequestBody(
          5, session->authentication_token,
          decoded_subscription->subscription_id, MonitoringMode::Sampling,
          {decoded_items->result.monitored_item_id})));
  ASSERT_TRUE(set_mode.has_value());
  const auto status =
      DecodeSingleResultStatus(*set_mode, kSetMonitoringModeResponseEncodingId);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest,
       HandlesDeleteMonitoredItemsAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const auto subscription = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeCreateSubscriptionRequestBody(
                     3, session->authentication_token,
                     {.publishing_interval_ms = 100,
                      .lifetime_count = 60,
                      .max_keep_alive_count = 3,
                      .publishing_enabled = true})));
  ASSERT_TRUE(subscription.has_value());
  const auto decoded_subscription =
      DecodeCreateSubscriptionResponse(*subscription);
  ASSERT_TRUE(decoded_subscription.has_value());

  const auto created_items = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateMonitoredItemsRequestBody(
          4, session->authentication_token,
          decoded_subscription->subscription_id,
          {{.item_to_monitor = {.node_id = NumericNode(11),
                                .attribute_id = opcua::AttributeId::Value},
            .requested_parameters = {.client_handle = 44,
                                     .sampling_interval_ms = 0,
                                     .queue_size = 1,
                                     .discard_oldest = true}}})));
  ASSERT_TRUE(created_items.has_value());
  const auto decoded_items = DecodeCreateMonitoredItemsResponse(*created_items);
  ASSERT_TRUE(decoded_items.has_value());

  const auto deleted = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeDeleteMonitoredItemsRequestBody(
                     5, session->authentication_token,
                     decoded_subscription->subscription_id,
                     {decoded_items->result.monitored_item_id})));
  ASSERT_TRUE(deleted.has_value());
  const auto status = DecodeSingleResultStatus(
      *deleted, kDeleteMonitoredItemsResponseEncodingId);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesCallAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  EXPECT_CALL(method_service_, Call(_, _, _, _))
      .WillOnce(Invoke([this](opcua::NodeId node_id, opcua::NodeId method_id,
                              std::vector<opcua::Variant> arguments,
                              opcua::NodeId user_id) {
        EXPECT_EQ(node_id, NumericNode(12));
        EXPECT_EQ(method_id, NumericNode(77));
        EXPECT_EQ(arguments.size(), 2u);
        if (arguments.size() >= 2) {
          EXPECT_DOUBLE_EQ(arguments[0].get<opcua::Double>(), 42.0);
          EXPECT_EQ(arguments[1].get<opcua::String>(), "go");
        }
        EXPECT_EQ(user_id, expected_user_id_);
        return opcua::MakeMethodCallResult(opcua::StatusCode::Good);
      }));

  const auto called = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCallRequestBody(
          3, session->authentication_token, NumericNode(12), NumericNode(77),
          {opcua::Double{42.0}, opcua::String{"go"}})));
  ASSERT_TRUE(called.has_value());
  const auto status = DecodeSingleCallResponseStatus(*called);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesDeleteNodesAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::DeleteNodesItem item{
      .node_id = NumericNode(12),
      .delete_target_references = true,
  };
  EXPECT_CALL(node_management_service_, DeleteNodes(_))
      .WillOnce(
          Invoke([&](std::vector<opcua::DeleteNodesItem> items)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].node_id, item.node_id);
            EXPECT_EQ(items[0].delete_target_references,
                      item.delete_target_references);
            co_return std::vector{opcua::StatusCode::Good};
          }));

  const auto deleted = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeDeleteNodesRequestBody(
                     3, session->authentication_token, item)));
  ASSERT_TRUE(deleted.has_value());
  const auto status = DecodeSingleDeleteNodesResponseStatus(*deleted);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesAddNodesAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::AddNodesItem item{
      .requested_id = NumericNode(50),
      .parent_id = NumericNode(51),
      .node_class = opcua::NodeClass::Variable,
      .type_definition_id = NumericNode(52),
      .attributes = opcua::NodeAttributes{}
                        .set_browse_name({"Flow", 2})
                        .set_display_name({u"Flow"})
                        .set_data_type(NumericNode(53)),
  };
  EXPECT_CALL(node_management_service_, AddNodes(_))
      .WillOnce(
          Invoke([&](std::vector<opcua::AddNodesItem> items)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::AddNodesResult>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(items[0], item);
            co_return std::vector{
                opcua::AddNodesResult{.status_code = opcua::StatusCode::Good,
                                      .added_node_id = NumericNode(54)}};
          }));

  const auto added = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeAddNodesRequestBody(
                     3, session->authentication_token, item)));
  ASSERT_TRUE(added.has_value());
  const auto result = DecodeSingleAddNodesResponseResult(*added);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status_code, opcua::StatusCode::Good);
  EXPECT_EQ(result->added_node_id, NumericNode(54));
}

TEST_F(ServiceDispatcherTest, HandlesDeleteReferencesAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::DeleteReferencesItem item{
      .source_node_id = NumericNode(60),
      .reference_type_id = NumericNode(61),
      .forward = true,
      .target_node_id = opcua::ExpandedNodeId{NumericNode(62)},
      .delete_bidirectional = true,
  };
  EXPECT_CALL(node_management_service_, DeleteReferences(_))
      .WillOnce(
          Invoke([&](std::vector<opcua::DeleteReferencesItem> items)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].source_node_id, item.source_node_id);
            EXPECT_EQ(items[0].reference_type_id, item.reference_type_id);
            EXPECT_EQ(items[0].forward, item.forward);
            EXPECT_EQ(items[0].target_node_id, item.target_node_id);
            EXPECT_EQ(items[0].delete_bidirectional, item.delete_bidirectional);
            co_return std::vector{opcua::StatusCode::Good};
          }));

  const auto deleted = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeDeleteReferencesRequestBody(
                     3, session->authentication_token, item)));
  ASSERT_TRUE(deleted.has_value());
  const auto status = DecodeSingleDeleteReferencesResponseStatus(*deleted);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, HandlesAddReferencesAfterActivatedSession) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  const auto created = opcua::WaitAwaitable(
      executor_,
      dispatcher.HandlePayload(EncodeCreateSessionRequestBody(1, 45000)));
  ASSERT_TRUE(created.has_value());
  const auto session = DecodeCreateSessionResponse(*created);
  ASSERT_TRUE(session.has_value());

  const auto activated = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeUserNameActivateRequestBody(
                     2, session->authentication_token, "operator", "secret")));
  ASSERT_TRUE(activated.has_value());

  const opcua::AddReferencesItem item{
      .source_node_id = NumericNode(70),
      .reference_type_id = NumericNode(71),
      .forward = true,
      .target_server_uri = "",
      .target_node_id = opcua::ExpandedNodeId{NumericNode(72)},
      .target_node_class = opcua::NodeClass::Object,
  };
  EXPECT_CALL(node_management_service_, AddReferences(_))
      .WillOnce(
          Invoke([&](std::vector<opcua::AddReferencesItem> items)
                     -> opcua::Awaitable<
                         opcua::StatusOr<std::vector<opcua::StatusCode>>> {
            EXPECT_EQ(items.size(), 1u);
            if (items.size() != 1u) {
              co_return opcua::Status{opcua::StatusCode::Bad};
            }
            EXPECT_EQ(items[0].source_node_id, item.source_node_id);
            EXPECT_EQ(items[0].reference_type_id, item.reference_type_id);
            EXPECT_EQ(items[0].forward, item.forward);
            EXPECT_EQ(items[0].target_server_uri, item.target_server_uri);
            EXPECT_EQ(items[0].target_node_id, item.target_node_id);
            EXPECT_EQ(items[0].target_node_class, item.target_node_class);
            co_return std::vector{opcua::StatusCode::Good};
          }));

  const auto added = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(EncodeAddReferencesRequestBody(
                     3, session->authentication_token, item)));
  ASSERT_TRUE(added.has_value());
  const auto status = DecodeSingleAddReferencesResponseStatus(*added);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 0u);
}

TEST_F(ServiceDispatcherTest, UnsupportedServiceReturnsServiceFault) {
  ServiceDispatcher dispatcher{
      {.runtime = runtime_, .connection = connection_}};

  // A request whose encoding id is not a service the server implements
  // (QueryFirst, i=615), but with a well-formed request header.
  std::vector<char> header;
  AppendRequestHeader(header, opcua::NodeId{}, /*request_handle=*/4242);
  std::vector<char> body;
  Encoder body_encoder{body};
  body_encoder.Encode(
      EncodedExtensionObject{.type_id = 615, .body = std::move(header)});

  const auto response = opcua::WaitAwaitable(
      executor_, dispatcher.HandlePayload(std::move(body)));
  ASSERT_TRUE(response.has_value());
  const auto decoded = DecodeServiceResponse(*response);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_handle, 4242u);
  const auto* fault = std::get_if<ServiceFault>(&decoded->body);
  ASSERT_NE(fault, nullptr);
  EXPECT_EQ(fault->status.code(), opcua::StatusCode::Bad_ServiceUnsupported);
}

}  // namespace
}  // namespace opcua::binary
