#include "opcua/transport/binary/service_codec.h"

#include "opcua/events/event_filter.h"
#include "opcua/transport/binary/codec_utils.h"
#include "opcua/ua/ua_encoding_ids.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace opcua::binary {
namespace {

constexpr std::uint32_t kReadRequestEncodingIdForTest = 631;
constexpr std::uint32_t kWriteRequestEncodingIdForTest = 673;
constexpr std::uint32_t kBrowseResponseEncodingIdForTest = 530;

// Every test below encodes a server-side response via the existing
// EncodeServiceResponse, then feeds the bytes through the new
// client-side DecodeServiceResponse and asserts the typed payload
// survives the round trip. This pins the inverse-pair contract without
// pulling in a live channel or transport.

template <typename Body>
DecodedResponse RoundTrip(std::uint32_t request_handle, Body body) {
  const auto encoded =
      EncodeServiceResponse(request_handle, ResponseBody{std::move(body)});
  EXPECT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceResponse(*encoded);
  EXPECT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_handle, request_handle);
  return *decoded;
}

void AppendRequestHeaderForTest(Encoder& encoder,
                                const opcua::NodeId& authentication_token,
                                std::uint32_t request_handle,
                                std::string_view audit_entry_id) {
  encoder.Encode(authentication_token);
  encoder.Encode(std::int64_t{0});
  encoder.Encode(request_handle);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(audit_entry_id);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(opcua::NodeId{});
  encoder.Encode(std::uint8_t{0x00});
}

void AppendResponseHeaderForTest(Encoder& encoder,
                                 std::uint32_t request_handle) {
  encoder.Encode(std::int64_t{0});
  encoder.Encode(request_handle);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(std::uint8_t{0});
  encoder.Encode(std::int32_t{0});
  encoder.Encode(opcua::NodeId{});
  encoder.Encode(std::uint8_t{0x00});
}

std::vector<char> WrapMessageForTest(std::uint32_t encoding_id,
                                     std::span<const char> payload) {
  std::vector<char> body;
  Encoder body_encoder{body};
  AppendMessage(body_encoder, encoding_id, payload);
  return body;
}

TEST(ServiceCodecTest, DecodeReadRequestSkipsIgnoredStringsAndDataEncoding) {
  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderForTest(encoder, opcua::NodeId{77, 3}, 41, "audit-entry");
  encoder.Encode(0.0);
  encoder.Encode(std::uint32_t{2});
  encoder.Encode(std::int32_t{1});
  encoder.Encode(opcua::NodeId{123, 4});
  encoder.Encode(static_cast<std::uint32_t>(opcua::AttributeId::Value));
  encoder.Encode(std::string_view{});
  encoder.Encode(opcua::QualifiedName{"Default Binary", 0});

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->header.authentication_token, (opcua::NodeId{77, 3}));
  EXPECT_EQ(decoded->header.request_handle, 41u);
  const auto& request = std::get<ua::ReadRequest>(decoded->body);
  ASSERT_EQ(request.nodes_to_read.size(), 1u);
  EXPECT_EQ(request.nodes_to_read[0].node_id, (opcua::NodeId{123, 4}));
  EXPECT_EQ(request.nodes_to_read[0].attribute_id,
            static_cast<opcua::UInt32>(opcua::AttributeId::Value));
}

TEST(ServiceCodecTest, DecodeReadRequestRejectsArrayCountExceedingBuffer) {
  // A node count far larger than the bytes that follow is a decode bomb: the
  // decoder must reject it rather than reserve/resize for ~2e9 entries.
  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderForTest(encoder, opcua::NodeId{77, 3}, 41, "audit-entry");
  encoder.Encode(0.0);
  encoder.Encode(std::uint32_t{2});
  encoder.Encode(std::int32_t{2000000000});

  EXPECT_FALSE(DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload)));
}

TEST(ServiceCodecTest, DecodeWriteRequestParsesIndexRange) {
  // The generated WriteValue models IndexRange as a real field (the
  // hand-written type dropped it and rejected a non-empty value); confirm the
  // codec now carries it through.
  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderForTest(encoder, opcua::NodeId{77, 3}, 42, "audit-entry");
  encoder.Encode(std::int32_t{1});
  encoder.Encode(opcua::NodeId{123, 4});
  encoder.Encode(static_cast<std::uint32_t>(opcua::AttributeId::Value));
  encoder.Encode(std::string_view{"0:1"});
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(opcua::Variant{std::int32_t{7}});

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kWriteRequestEncodingIdForTest, payload));
  ASSERT_TRUE(decoded);
  const auto* write = std::get_if<ua::WriteRequest>(&decoded->body);
  ASSERT_NE(write, nullptr);
  ASSERT_EQ(write->nodes_to_write.size(), 1u);
  EXPECT_EQ(write->nodes_to_write[0].index_range, "0:1");
  EXPECT_EQ(write->nodes_to_write[0].value.value.get<opcua::Int32>(), 7);
}

TEST(ServiceCodecTest, DecodeBrowseResponseReadsReferenceFields) {
  std::vector<char> payload;
  Encoder encoder{payload};
  AppendResponseHeaderForTest(encoder, 43);
  encoder.Encode(std::int32_t{1});
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(opcua::ByteString{});
  encoder.Encode(std::int32_t{1});
  encoder.Encode(opcua::NodeId{35});
  encoder.Encode(true);
  encoder.Encode(opcua::ExpandedNodeId{opcua::NodeId{456, 4}});
  encoder.Encode(opcua::QualifiedName{"Pump", 4});
  encoder.Encode(opcua::LocalizedText{u"Pump display"});
  encoder.Encode(std::uint32_t{1});
  encoder.Encode(opcua::ExpandedNodeId{});
  encoder.Encode(std::int32_t{-1});

  const auto decoded = DecodeServiceResponse(
      WrapMessageForTest(kBrowseResponseEncodingIdForTest, payload));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->request_handle, 43u);
  const auto& response = std::get<ua::BrowseResponse>(decoded->body);
  ASSERT_EQ(response.results.size(), 1u);
  ASSERT_EQ(response.results[0].references.size(), 1u);
  EXPECT_EQ(response.results[0].references[0].reference_type_id,
            opcua::NodeId{35});
  EXPECT_TRUE(response.results[0].references[0].is_forward);
  EXPECT_EQ(response.results[0].references[0].node_id.node_id(),
            (opcua::NodeId{456, 4}));
}

TEST(ServiceCodecTest, FindServersResponseRoundTrip) {
  FindServersResponse response{
      .servers = {ApplicationDescription{
          .application_uri = "urn:test:server",
          .product_uri = "urn:test:product",
          .application_name = opcua::LocalizedText{u"Test Server"},
          .application_type = ApplicationType::Server,
          .discovery_urls = {"opc.tcp://localhost:4840"},
      }}};

  const auto decoded = RoundTrip(44, std::move(response));

  const auto& typed = std::get<FindServersResponse>(decoded.body);
  ASSERT_EQ(typed.servers.size(), 1u);
  EXPECT_EQ(typed.servers[0].application_uri, "urn:test:server");
  EXPECT_EQ(typed.servers[0].product_uri, "urn:test:product");
  EXPECT_EQ(typed.servers[0].application_name,
            opcua::LocalizedText{u"Test Server"});
  EXPECT_EQ(typed.servers[0].application_type, ApplicationType::Server);
  EXPECT_THAT(typed.servers[0].discovery_urls,
              testing::ElementsAre("opc.tcp://localhost:4840"));
}

TEST(ServiceCodecTest, RegisterServerRequestRoundTrip) {
  RegisterServerRequest request{
      .server = RegisteredServer{
          .server_uri = "urn:edge:modbus",
          .product_uri = "urn:scada:edge",
          .server_names = {opcua::LocalizedText{u"Modbus Edge"}},
          .server_type = ApplicationType::Server,
          .discovery_urls = {"opc.tcp://edge:4841"},
          .is_online = true}};
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<RegisterServerRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->server.server_uri, "urn:edge:modbus");
  EXPECT_EQ(typed->server.product_uri, "urn:scada:edge");
  ASSERT_EQ(typed->server.server_names.size(), 1u);
  EXPECT_EQ(typed->server.server_names[0],
            opcua::LocalizedText{u"Modbus Edge"});
  EXPECT_EQ(typed->server.server_type, ApplicationType::Server);
  EXPECT_THAT(typed->server.discovery_urls,
              testing::ElementsAre("opc.tcp://edge:4841"));
  EXPECT_TRUE(typed->server.is_online);
}

TEST(ServiceCodecTest, RegisterServerResponseRoundTrip) {
  RegisterServerResponse response{.status = StatusCode::Good};
  const auto decoded = RoundTrip(11, std::move(response));
  const auto& typed = std::get<RegisterServerResponse>(decoded.body);
  EXPECT_FALSE(typed.status.bad());
}

// OPC UA Part 4 §5.4.6 RegisterServer2: the discoveryConfiguration
// (MdnsDiscoveryConfiguration + its serverCapabilities, Part 4 §7.8) must
// round-trip so a historian's "HD" capability reaches the discovery target.
TEST(ServiceCodecTest, RegisterServer2RequestRoundTrip) {
  RegisterServer2Request request{
      .server =
          RegisteredServer{.server_uri = "urn:site:historian",
                           .product_uri = "urn:scada:historian",
                           .server_names = {opcua::LocalizedText{u"Historian"}},
                           .server_type = ApplicationType::Server,
                           .discovery_urls = {"opc.tcp://historian:4842"},
                           .is_online = true}};
  request.discovery_configuration.push_back(MdnsDiscoveryConfiguration{
      .mdns_server_name = "urn:site:historian", .server_capabilities = {"HD"}});
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<RegisterServer2Request>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->server.server_uri, "urn:site:historian");
  EXPECT_TRUE(typed->server.is_online);
  ASSERT_EQ(typed->discovery_configuration.size(), 1u);
  ASSERT_TRUE(typed->discovery_configuration[0].has_value());
  EXPECT_EQ(typed->discovery_configuration[0]->mdns_server_name,
            "urn:site:historian");
  EXPECT_THAT(typed->discovery_configuration[0]->server_capabilities,
              testing::ElementsAre("HD"));
}

TEST(ServiceCodecTest, RegisterServer2ResponseRoundTrip) {
  RegisterServer2Response response{
      .status = StatusCode::Good,
      .configuration_results = {StatusCode::Good,
                                StatusCode::Bad_NotSupported}};
  const auto decoded = RoundTrip(12, std::move(response));
  const auto& typed = std::get<RegisterServer2Response>(decoded.body);
  EXPECT_FALSE(typed.status.bad());
  EXPECT_THAT(
      typed.configuration_results,
      testing::ElementsAre(StatusCode::Good, StatusCode::Bad_NotSupported));
}

TEST(ServiceCodecTest, GetEndpointsResponseRoundTrip) {
  GetEndpointsResponse response{
      .endpoints = {EndpointDescription{
          .endpoint_url = "opc.tcp://localhost:4840",
          .server =
              ApplicationDescription{
                  .application_uri = "urn:test:server",
                  .product_uri = "urn:test:product",
                  .application_name = opcua::LocalizedText{u"Test Server"},
                  .application_type = ApplicationType::Server,
                  .discovery_urls = {"opc.tcp://localhost:4840"},
              },
          .security_mode = MessageSecurityMode::None,
          .security_policy_uri =
              "http://opcfoundation.org/UA/SecurityPolicy#None",
          .user_identity_tokens =
              {UserTokenPolicy{.policy_id = "anonymous",
                               .token_type = UserTokenType::Anonymous},
               UserTokenPolicy{
                   .policy_id = "username",
                   .token_type = UserTokenType::UserName,
                   .security_policy_uri =
                       "http://opcfoundation.org/UA/SecurityPolicy#None"}},
          .transport_profile_uri =
              "http://opcfoundation.org/UA-Profile/Transport/"
              "uatcp-uasc-uabinary",
      }}};

  const auto decoded = RoundTrip(45, std::move(response));

  const auto& typed = std::get<GetEndpointsResponse>(decoded.body);
  ASSERT_EQ(typed.endpoints.size(), 1u);
  const auto& endpoint = typed.endpoints[0];
  EXPECT_EQ(endpoint.endpoint_url, "opc.tcp://localhost:4840");
  EXPECT_EQ(endpoint.security_mode, MessageSecurityMode::None);
  EXPECT_EQ(endpoint.security_policy_uri,
            "http://opcfoundation.org/UA/SecurityPolicy#None");
  EXPECT_THAT(endpoint.user_identity_tokens,
              testing::ElementsAre(testing::Field(&UserTokenPolicy::token_type,
                                                  UserTokenType::Anonymous),
                                   testing::Field(&UserTokenPolicy::token_type,
                                                  UserTokenType::UserName)));
  EXPECT_EQ(endpoint.transport_profile_uri,
            "http://opcfoundation.org/UA-Profile/Transport/"
            "uatcp-uasc-uabinary");
}

TEST(ServiceCodecTest, CreateSessionResponseRoundTrip) {
  CreateSessionResponse response{
      .status = opcua::StatusCode::Good,
      .session_id = opcua::NodeId{42},
      .authentication_token = opcua::NodeId{43},
      .server_nonce = opcua::ByteString{'n', 'o', 'n', 'c', 'e'},
      .revised_timeout = opcua::Duration::FromMilliseconds(60000),
  };
  const auto decoded = RoundTrip(17, response);
  const auto& typed = std::get<CreateSessionResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  EXPECT_EQ(typed.session_id, response.session_id);
  EXPECT_EQ(typed.authentication_token, response.authentication_token);
  EXPECT_EQ(typed.revised_timeout.InMilliseconds(), 60000);
  EXPECT_EQ(typed.server_nonce, response.server_nonce);
}

// The client now sends a non-empty client certificate and nonce in
// CreateSession; the server-side decoder must still parse the message (it skips
// those fields). This pins the wire framing so a secured request stays
// decodable.
TEST(ServiceCodecTest,
     CreateSessionRequestWithClientCredentialsStaysDecodable) {
  CreateSessionRequest request{
      .requested_timeout = opcua::Duration::FromSeconds(120),
      .client_certificate = opcua::ByteString{'c', 'e', 'r', 't'},
      .client_nonce = opcua::ByteString{'n', 'o', 'n', 'c', 'e'},
  };
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<CreateSessionRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->requested_timeout.InMilliseconds(), 120000);
}

TEST(ServiceCodecTest, RegisterNodesRequestRoundTrip) {
  RegisterNodesRequest request{
      .nodes_to_register = {opcua::NodeId{12},
                            opcua::NodeId{opcua::String{"Item"}, 3}}};
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<RegisterNodesRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->nodes_to_register, request.nodes_to_register);
}

TEST(ServiceCodecTest, HistoryUpdateRequestRoundTrip) {
  DataValue value;
  value.value = Variant{std::int32_t{42}};
  value.status_code = StatusCode::Good;
  const opcua::NodeId tag_node_id{opcua::String{"Tag"}, 2};
  HistoryUpdateRequest request{
      .details = UpdateDataDetails{
          .node_id = tag_node_id,
          .perform_insert_replace = PerformUpdateType::Replace,
          .values = {value}}};
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<HistoryUpdateRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  const auto* data = std::get_if<UpdateDataDetails>(&typed->details);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->node_id, tag_node_id);
  EXPECT_EQ(data->perform_insert_replace, PerformUpdateType::Replace);
  ASSERT_EQ(data->values.size(), 1u);
  EXPECT_EQ(data->values[0].value, value.value);
}

// An UpdateEventDetails detail round-trips through the same HistoryUpdate
// service, dispatched by its extension-object type id. Only the default
// BaseEventType select clauses round-trip
// (EventId/EventType/SourceNode/Time/Message/Severity).
TEST(ServiceCodecTest, HistoryUpdateEventRequestRoundTrip) {
  const opcua::NodeId tag_node_id{opcua::String{"Tag"}, 2};
  const opcua::NodeId alarm_type_id{opcua::String{"AlarmType"}, 3};
  const opcua::NodeId user_node_id{opcua::String{"User"}, 4};
  opcua::Event event;
  event.event_id = 7;
  event.event_type_id = alarm_type_id;
  event.source_node_id = tag_node_id;
  event.severity = 700;
  event.message = u"boom";
  // SCADA-extension fields that must also round-trip.
  event.value = opcua::Variant{42.0};
  event.qualifier = opcua::Qualifier{0x42};
  event.change_mask = 5;
  event.user_id = user_node_id;
  event.acked = true;
  HistoryUpdateRequest request{
      .details = UpdateEventDetails{
          .node_id = tag_node_id,
          .perform_insert_replace = PerformUpdateType::Insert,
          .events = {event}}};
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<HistoryUpdateRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  const auto* event_details = std::get_if<UpdateEventDetails>(&typed->details);
  ASSERT_NE(event_details, nullptr);
  EXPECT_EQ(event_details->node_id, tag_node_id);
  EXPECT_EQ(event_details->perform_insert_replace, PerformUpdateType::Insert);
  ASSERT_EQ(event_details->events.size(), 1u);
  const auto& decoded_event = event_details->events[0];
  EXPECT_EQ(decoded_event.event_id, 7u);
  EXPECT_EQ(decoded_event.event_type_id, alarm_type_id);
  EXPECT_EQ(decoded_event.source_node_id, tag_node_id);
  EXPECT_EQ(decoded_event.severity, 700u);
  EXPECT_EQ(decoded_event.message, event.message);
  EXPECT_EQ(decoded_event.value.as_double(), 42.0);
  EXPECT_EQ(decoded_event.qualifier.raw(), 0x42u);
  EXPECT_EQ(decoded_event.change_mask, 5u);
  EXPECT_EQ(decoded_event.user_id, user_node_id);
  EXPECT_TRUE(decoded_event.acked);
}

TEST(ServiceCodecTest, HistoryUpdateResponseRoundTrip) {
  HistoryUpdateResponse response{
      .result = {
          .status = StatusCode::Good,
          .operation_results = {StatusCode::Good,
                                StatusCode::Bad_HistoryOperationInvalid}}};
  const auto encoded = EncodeServiceResponse(7, ResponseBody{response});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceResponse(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<HistoryUpdateResponse>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->result.operation_results.size(), 2u);
  EXPECT_EQ(typed->result.operation_results[0], StatusCode::Good);
  EXPECT_EQ(typed->result.operation_results[1],
            StatusCode::Bad_HistoryOperationInvalid);
}

TEST(ServiceCodecTest, HistoryReadRawResponseRoundTrip) {
  DataValue value;
  value.value = Variant{std::int32_t{7}};
  value.status_code = StatusCode::Good;
  HistoryReadRawResponse response{
      .result = {.status = StatusCode::Good,
                 .values = {value},
                 .continuation_point = opcua::ByteString{'c', 'p'}}};
  const auto encoded = EncodeServiceResponse(9, ResponseBody{response});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceResponse(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<HistoryReadRawResponse>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->result.values.size(), 1u);
  EXPECT_EQ(typed->result.values[0].value, value.value);
  EXPECT_EQ(typed->result.continuation_point,
            response.result.continuation_point);
}

TEST(ServiceCodecTest, HistoryReadEventsResponseRoundTrip) {
  Event event;
  event.event_id = 11;
  event.event_type_id = opcua::NodeId{501};
  event.source_node_id = opcua::NodeId{opcua::String{"Pump"}, 4};
  event.time = opcua::DateTime::Now();
  event.message = opcua::LocalizedText{u"alarm"};
  event.severity = 900;

  HistoryReadEventsResponse response{
      .result = {.status = StatusCode::Good, .events = {event}}};
  const auto encoded =
      EncodeHistoryReadEventsResponse(7, response, DefaultEventFieldPaths());
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceResponse(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<HistoryReadEventsResponse>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->result.events.size(), 1u);
  // The default BaseEventType select clauses recover these fields.
  EXPECT_EQ(typed->result.events[0].event_id, 11u);
  EXPECT_EQ(typed->result.events[0].event_type_id, event.event_type_id);
  EXPECT_EQ(typed->result.events[0].source_node_id, event.source_node_id);
  EXPECT_EQ(typed->result.events[0].time, event.time);
  EXPECT_EQ(typed->result.events[0].message, event.message);
  EXPECT_EQ(typed->result.events[0].severity, 900u);
}

TEST(ServiceCodecTest, RegisterNodesResponseRoundTrip) {
  RegisterNodesResponse response{
      .registered_node_ids = {opcua::NodeId{12}, opcua::NodeId{99}}};
  const auto encoded = EncodeServiceResponse(7, ResponseBody{response});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceResponse(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<RegisterNodesResponse>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->registered_node_ids, response.registered_node_ids);
}

// Likewise, a non-empty ActivateSession clientSignature must keep the request
// decodable (the server skips the SignatureData).
TEST(ServiceCodecTest, ActivateSessionRequestWithSignatureStaysDecodable) {
  ActivateSessionRequest request{
      .session_id = opcua::NodeId{7},
      .authentication_token = opcua::NodeId{8},
      .allow_anonymous = true,
      .client_signature_algorithm =
          "http://www.w3.org/2001/04/xmldsig-more#rsa-sha256",
      .client_signature = opcua::ByteString{1, 2, 3, 4},
  };
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<ActivateSessionRequest>(decoded->body));
  const auto& typed = std::get<ActivateSessionRequest>(decoded->body);
  EXPECT_TRUE(typed.allow_anonymous);
  EXPECT_EQ(typed.client_signature_algorithm,
            request.client_signature_algorithm);
  EXPECT_EQ(typed.client_signature, request.client_signature);
}

// A UserNameIdentityToken with a cleartext password must round-trip through the
// generated ActivateSession codec: the server side reconstructs the credentials
// from the identity-token ExtensionObject (OPC UA Part 4 §7.36.4).
TEST(ServiceCodecTest, ActivateSessionUserNameTokenRoundTrips) {
  ActivateSessionRequest request{
      .authentication_token = opcua::NodeId{8},
      .user_name = opcua::ToLocalizedText(std::string{"operator"}),
      .password = opcua::ToLocalizedText(std::string{"s3cret"}),
      .allow_anonymous = false,
  };
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<ActivateSessionRequest>(decoded->body));
  const auto& typed = std::get<ActivateSessionRequest>(decoded->body);
  EXPECT_FALSE(typed.allow_anonymous);
  ASSERT_TRUE(typed.user_name.has_value());
  EXPECT_EQ(opcua::ToString(*typed.user_name), "operator");
  ASSERT_TRUE(typed.password.has_value());
  EXPECT_EQ(opcua::ToString(*typed.password), "s3cret");
  // A cleartext token carries no encryption algorithm, so nothing is routed to
  // the encrypted-password path.
  EXPECT_TRUE(typed.password_encryption_algorithm.empty());
  EXPECT_TRUE(typed.encrypted_password.empty());
}

TEST(ServiceCodecTest, AddNodesResponseRoundTrip) {
  ua::AddNodesResponse response;
  response.results = {ua::AddNodesResult{
      .status_code = Status{opcua::StatusCode::Good},
      .added_node_id = opcua::NodeId{101, 6},
  }};

  const auto decoded = RoundTrip(18, response);

  const auto& typed = std::get<ua::AddNodesResponse>(decoded.body);
  EXPECT_TRUE(typed.response_header.service_result.good());
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_EQ(typed.results[0].status_code.code(), opcua::StatusCode::Good);
  EXPECT_EQ(typed.results[0].added_node_id, (opcua::NodeId{101, 6}));
}

TEST(ServiceCodecTest, ActivateSessionResponseRoundTrip) {
  ActivateSessionResponse response{.status = opcua::StatusCode::Good};
  const auto decoded = RoundTrip(5, response);
  const auto& typed = std::get<ActivateSessionResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
}

TEST(ServiceCodecTest, CloseSessionResponseRoundTrip) {
  CloseSessionResponse response{.status = opcua::StatusCode::Good};
  const auto decoded = RoundTrip(9, response);
  const auto& typed = std::get<CloseSessionResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
}

TEST(ServiceCodecTest, ReadResponseRoundTrip) {
  opcua::DataValue bad_value{opcua::Variant{std::int32_t{-1}}, {}, {}, {}};
  bad_value.status_code = opcua::StatusCode::Bad;
  ua::ReadResponse response{
      .results = {opcua::DataValue{
                      opcua::Variant{std::int32_t{42}}, {}, {}, {}},
                  bad_value},
  };
  const auto decoded = RoundTrip(11, response);
  const auto& typed = std::get<ua::ReadResponse>(decoded.body);
  EXPECT_TRUE(typed.response_header.service_result.good());
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_EQ(typed.results[0].value, response.results[0].value);
  EXPECT_TRUE(opcua::IsGood(typed.results[0].status_code));
  EXPECT_EQ(typed.results[1].value, response.results[1].value);
  EXPECT_EQ(typed.results[1].status_code, opcua::StatusCode::Bad);
}

TEST(ServiceCodecTest, WriteResponseRoundTrip) {
  ua::WriteResponse response;
  response.results = {Status{opcua::StatusCode::Good},
                      Status{opcua::StatusCode::Bad_AttributeIdInvalid}};
  const auto decoded = RoundTrip(12, response);
  const auto& typed = std::get<ua::WriteResponse>(decoded.body);
  EXPECT_TRUE(typed.response_header.service_result.good());
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_EQ(typed.results[0].code(), opcua::StatusCode::Good);
  EXPECT_EQ(typed.results[1].code(), opcua::StatusCode::Bad_AttributeIdInvalid);
}

TEST(ServiceCodecTest, BrowseResponseRoundTrip) {
  const opcua::ByteString opaque_id{'o', 'p', 'a', 'q', 'u', 'e'};
  ua::BrowseResponse response{
      .results =
          {ua::BrowseResult{
               .status_code = Status{opcua::StatusCode::Good},
               .continuation_point = opcua::ByteString{'c', 'p'},
               .references =
                   {ua::ReferenceDescription{
                        .reference_type_id = opcua::NodeId{35},
                        .is_forward = true,
                        .node_id = opcua::ExpandedNodeId{opcua::NodeId{100}},
                        .node_class = ua::NodeClass::Variable,
                    },
                    ua::ReferenceDescription{
                        .reference_type_id = opcua::NodeId{35},
                        .is_forward = true,
                        .node_id = opcua::ExpandedNodeId{opcua::NodeId{
                            opcua::String{"File.txt"}, 7}},
                    },
                    ua::ReferenceDescription{
                        .reference_type_id = opcua::NodeId{35},
                        .is_forward = true,
                        .node_id =
                            opcua::ExpandedNodeId{opcua::NodeId{opaque_id, 8}},
                    },
                    ua::ReferenceDescription{
                        .reference_type_id = opcua::NodeId{35},
                        .is_forward = true,
                        .node_id = opcua::ExpandedNodeId{opcua::NodeId{200}},
                        .browse_name = opcua::QualifiedName{"Widget", 2},
                        .display_name = opcua::LocalizedText{u"Widget"},
                        .node_class = ua::NodeClass::Object,
                        .type_definition =
                            opcua::ExpandedNodeId{opcua::NodeId{58}},
                    }},
           },
           ua::BrowseResult{
               .status_code = Status{opcua::StatusCode::Bad_NothingToDo},
           }},
  };
  const auto decoded = RoundTrip(13, response);
  const auto& typed = std::get<ua::BrowseResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_TRUE(typed.results[0].status_code.good());
  EXPECT_EQ(typed.results[0].continuation_point,
            response.results[0].continuation_point);
  ASSERT_EQ(typed.results[0].references.size(), 4u);
  EXPECT_EQ(typed.results[0].references[0].reference_type_id,
            opcua::NodeId{35});
  EXPECT_TRUE(typed.results[0].references[0].is_forward);
  EXPECT_EQ(typed.results[0].references[0].node_id.node_id(),
            opcua::NodeId{100});
  EXPECT_EQ(typed.results[0].references[0].node_class, ua::NodeClass::Variable);
  EXPECT_EQ(typed.results[0].references[1].node_id.node_id(),
            (opcua::NodeId{opcua::String{"File.txt"}, 7}));
  EXPECT_EQ(typed.results[0].references[2].node_id.node_id(),
            (opcua::NodeId{opaque_id, 8}));
  // BrowseName / DisplayName / TypeDefinition survive the round trip.
  EXPECT_EQ(typed.results[0].references[3].browse_name,
            (opcua::QualifiedName{"Widget", 2}));
  EXPECT_EQ(typed.results[0].references[3].display_name,
            opcua::LocalizedText{u"Widget"});
  EXPECT_EQ(typed.results[0].references[3].type_definition.node_id(),
            opcua::NodeId{58});
  EXPECT_EQ(typed.results[1].status_code.code(),
            opcua::StatusCode::Bad_NothingToDo);
  EXPECT_TRUE(typed.results[1].references.empty());
}

TEST(ServiceCodecTest, BrowseNextResponseRoundTrip) {
  ua::BrowseNextResponse response{
      .results = {ua::BrowseResult{.status_code =
                                       Status{opcua::StatusCode::Good}}},
  };
  const auto decoded = RoundTrip(14, response);
  const auto& typed = std::get<ua::BrowseNextResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(typed.results[0].status_code.good());
}

TEST(ServiceCodecTest, TranslateBrowsePathsResponseRoundTrip) {
  TranslateBrowsePathsResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::BrowsePathResult{
          .status_code = opcua::StatusCode::Good,
          .targets = {opcua::BrowsePathTarget{
              .target_id = opcua::ExpandedNodeId{opcua::NodeId{77}},
              .remaining_path_index = 0,
          }},
      }},
  };
  const auto decoded = RoundTrip(15, response);
  const auto& typed = std::get<TranslateBrowsePathsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(opcua::IsGood(typed.results[0].status_code));
  ASSERT_EQ(typed.results[0].targets.size(), 1u);
  EXPECT_EQ(typed.results[0].targets[0].target_id.node_id(), opcua::NodeId{77});
}

TEST(ServiceCodecTest, CallResponseRoundTrip) {
  ua::CallResponse response;
  response.results.push_back(
      {.status_code = opcua::StatusCode::Good,
       .input_argument_results = {opcua::Status{opcua::StatusCode::Good}},
       .output_arguments = {opcua::Variant{std::int32_t{100}},
                            opcua::Variant{std::int32_t{200}}}});
  const auto decoded = RoundTrip(16, response);
  const auto& typed = std::get<ua::CallResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(typed.results[0].status_code.good());
  ASSERT_EQ(typed.results[0].input_argument_results.size(), 1u);
  EXPECT_EQ(typed.results[0].input_argument_results[0].code(),
            opcua::StatusCode::Good);
  ASSERT_EQ(typed.results[0].output_arguments.size(), 2u);
  EXPECT_EQ(typed.results[0].output_arguments[0],
            opcua::Variant{std::int32_t{100}});
  EXPECT_EQ(typed.results[0].output_arguments[1],
            opcua::Variant{std::int32_t{200}});
}

TEST(ServiceCodecTest, CreateSubscriptionResponseRoundTrip) {
  CreateSubscriptionResponse response{
      .status = opcua::StatusCode::Good,
      .subscription_id = 7,
      .revised_publishing_interval_ms = 500.0,
      .revised_lifetime_count = 1200,
      .revised_max_keep_alive_count = 20,
  };
  const auto decoded = RoundTrip(18, response);
  const auto& typed = std::get<CreateSubscriptionResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  EXPECT_EQ(typed.subscription_id, 7u);
  EXPECT_EQ(typed.revised_publishing_interval_ms, 500.0);
  EXPECT_EQ(typed.revised_lifetime_count, 1200u);
  EXPECT_EQ(typed.revised_max_keep_alive_count, 20u);
}

TEST(ServiceCodecTest, ModifySubscriptionResponseRoundTrip) {
  ModifySubscriptionResponse response{
      .status = opcua::StatusCode::Good,
      .revised_publishing_interval_ms = 1000.0,
      .revised_lifetime_count = 600,
      .revised_max_keep_alive_count = 10,
  };
  const auto decoded = RoundTrip(19, response);
  const auto& typed = std::get<ModifySubscriptionResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  EXPECT_EQ(typed.revised_publishing_interval_ms, 1000.0);
  EXPECT_EQ(typed.revised_lifetime_count, 600u);
  EXPECT_EQ(typed.revised_max_keep_alive_count, 10u);
}

TEST(ServiceCodecTest, DeleteSubscriptionsResponseRoundTrip) {
  // Migrated to the generated type: results are full Status values, the
  // service result lives in the ResponseHeader.
  ua::DeleteSubscriptionsResponse response;
  response.results = {Status{opcua::StatusCode::Good},
                      Status{opcua::StatusCode::Bad}};
  const auto decoded = RoundTrip(20, response);
  const auto& typed = std::get<ua::DeleteSubscriptionsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_TRUE(typed.results[0].good());
  EXPECT_TRUE(typed.results[1].bad());
}

// Wire guard for the first service migrated to the generated codec: the
// request must still go out under the DeleteSubscriptions DefaultBinary
// encoding id (847), and the envelope fields the hand-written path carried
// separately (auth token, request handle, traceparent) must survive the round
// trip now that they live in the message's embedded RequestHeader. Together
// with UaServiceHeaderTest.GeneratedRequestHeaderMatchesHandWritten (which
// pins the header bytes), this locks the DeleteSubscriptions wire against the
// pre-migration form.
TEST(ServiceCodecTest, DeleteSubscriptionsRequestKeepsItsWire) {
  const ServiceRequestHeader header{.authentication_token = opcua::NodeId{5, 1},
                                    .request_handle = 9,
                                    .trace_parent = "00-abc-def-01"};
  const auto encoded = EncodeServiceRequest(
      header,
      RequestBody{ua::DeleteSubscriptionsRequest{.subscription_ids = {7, 8}}});
  ASSERT_TRUE(encoded.has_value());

  // First field on the wire is the message's DefaultBinary encoding id.
  Decoder message_decoder{*encoded};
  const auto message = ReadMessage(message_decoder);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->first,
            ua::binary_encoding_id::kDeleteSubscriptionsRequest);

  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->header.authentication_token, (opcua::NodeId{5, 1}));
  EXPECT_EQ(decoded->header.request_handle, 9u);
  EXPECT_EQ(decoded->header.trace_parent, "00-abc-def-01");
  const auto& request = std::get<ua::DeleteSubscriptionsRequest>(decoded->body);
  EXPECT_EQ(request.subscription_ids, (std::vector<UInt32>{7u, 8u}));
}

TEST(ServiceCodecTest, SetPublishingModeResponseRoundTrip) {
  ua::SetPublishingModeResponse response;
  response.results = {Status{opcua::StatusCode::Good}};
  const auto decoded = RoundTrip(21, response);
  const auto& typed = std::get<ua::SetPublishingModeResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(typed.results[0].good());
}

// Wire guard for the generated SetPublishingMode request (see the
// DeleteSubscriptions equivalent): the encoding id (799) and the envelope
// fields, plus the publishing_enabled flag, survive the round trip.
TEST(ServiceCodecTest, SetPublishingModeRequestKeepsItsWire) {
  const ServiceRequestHeader header{.authentication_token = opcua::NodeId{5, 1},
                                    .request_handle = 9};
  const auto encoded = EncodeServiceRequest(
      header, RequestBody{ua::SetPublishingModeRequest{
                  .publishing_enabled = false, .subscription_ids = {7}}});
  ASSERT_TRUE(encoded.has_value());

  Decoder message_decoder{*encoded};
  const auto message = ReadMessage(message_decoder);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->first, ua::binary_encoding_id::kSetPublishingModeRequest);

  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->header.request_handle, 9u);
  const auto& request = std::get<ua::SetPublishingModeRequest>(decoded->body);
  EXPECT_FALSE(request.publishing_enabled);
  EXPECT_EQ(request.subscription_ids, (std::vector<UInt32>{7u}));
}

TEST(ServiceCodecTest, CreateMonitoredItemsResponseRoundTrip) {
  CreateMonitoredItemsResponse response{
      .status = opcua::StatusCode::Good,
      .results = {MonitoredItemCreateResult{
                      .status = opcua::StatusCode::Good,
                      .monitored_item_id = 101,
                      .revised_sampling_interval_ms = 500.0,
                      .revised_queue_size = 8,
                  },
                  MonitoredItemCreateResult{
                      .status = opcua::StatusCode::Bad,
                      .monitored_item_id = 0,
                      .revised_sampling_interval_ms = 0.0,
                      .revised_queue_size = 0,
                  }},
  };
  const auto decoded = RoundTrip(22, response);
  const auto& typed = std::get<CreateMonitoredItemsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_TRUE(typed.results[0].status.good());
  EXPECT_EQ(typed.results[0].monitored_item_id, 101u);
  EXPECT_EQ(typed.results[0].revised_sampling_interval_ms, 500.0);
  EXPECT_EQ(typed.results[0].revised_queue_size, 8u);
  EXPECT_TRUE(typed.results[1].status.bad());
}

TEST(ServiceCodecTest, ModifyMonitoredItemsResponseRoundTrip) {
  ModifyMonitoredItemsResponse response{
      .status = opcua::StatusCode::Good,
      .results = {MonitoredItemModifyResult{
          .status = opcua::StatusCode::Good,
          .revised_sampling_interval_ms = 250.0,
          .revised_queue_size = 4,
      }},
  };
  const auto decoded = RoundTrip(23, response);
  const auto& typed = std::get<ModifyMonitoredItemsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(typed.results[0].status.good());
  EXPECT_EQ(typed.results[0].revised_sampling_interval_ms, 250.0);
  EXPECT_EQ(typed.results[0].revised_queue_size, 4u);
}

TEST(ServiceCodecTest, DeleteMonitoredItemsResponseRoundTrip) {
  ua::DeleteMonitoredItemsResponse response;
  response.results = {Status{opcua::StatusCode::Good},
                      Status{opcua::StatusCode::Bad_MonitoredItemIdInvalid}};
  const auto decoded = RoundTrip(24, response);
  const auto& typed = std::get<ua::DeleteMonitoredItemsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_TRUE(typed.results[0].good());
  EXPECT_EQ(typed.results[1].code(),
            opcua::StatusCode::Bad_MonitoredItemIdInvalid);
}

TEST(ServiceCodecTest, SetMonitoringModeResponseRoundTrip) {
  ua::SetMonitoringModeResponse response;
  response.results = {Status{opcua::StatusCode::Good}};
  const auto decoded = RoundTrip(25, response);
  const auto& typed = std::get<ua::SetMonitoringModeResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(typed.results[0].good());
}

TEST(ServiceCodecTest, PublishResponseRoundTripDataChange) {
  PublishResponse response{
      .status = opcua::StatusCode::Good,
      .subscription_id = 42,
      .results = {opcua::StatusCode::Good},
      .more_notifications = false,
      .notification_message =
          {.sequence_number = 7,
           .publish_time = opcua::DateTime{},
           .notification_data = {DataChangeNotification{
               .monitored_items =
                   {{.client_handle = 1,
                     .value =
                         opcua::DataValue{
                             opcua::Variant{std::int32_t{99}}, {}, {}, {}}}}}}},
      .available_sequence_numbers = {5, 6, 7},
  };
  const auto decoded = RoundTrip(26, response);
  const auto& typed = std::get<PublishResponse>(decoded.body);
  EXPECT_EQ(typed.subscription_id, 42u);
  EXPECT_EQ(typed.available_sequence_numbers,
            response.available_sequence_numbers);
  EXPECT_FALSE(typed.more_notifications);
  EXPECT_EQ(typed.notification_message.sequence_number, 7u);
  ASSERT_EQ(typed.notification_message.notification_data.size(), 1u);
  const auto* change = std::get_if<DataChangeNotification>(
      &typed.notification_message.notification_data[0]);
  ASSERT_NE(change, nullptr);
  ASSERT_EQ(change->monitored_items.size(), 1u);
  EXPECT_EQ(change->monitored_items[0].client_handle, 1u);
  EXPECT_EQ(change->monitored_items[0].value.value,
            opcua::Variant{std::int32_t{99}});
}

TEST(ServiceCodecTest, PublishResponseRoundTripStatusChange) {
  PublishResponse response{
      .status = opcua::StatusCode::Good,
      .subscription_id = 1,
      .notification_message =
          {.sequence_number = 1,
           .notification_data = {StatusChangeNotification{
               .status = opcua::StatusCode::Bad_SubscriptionIdInvalid}}},
  };
  const auto decoded = RoundTrip(27, response);
  const auto& typed = std::get<PublishResponse>(decoded.body);
  ASSERT_EQ(typed.notification_message.notification_data.size(), 1u);
  const auto* change = std::get_if<StatusChangeNotification>(
      &typed.notification_message.notification_data[0]);
  ASSERT_NE(change, nullptr);
  EXPECT_EQ(change->status, opcua::StatusCode::Bad_SubscriptionIdInvalid);
}

TEST(ServiceCodecTest, RepublishResponseRoundTrip) {
  RepublishResponse response{
      .status = opcua::StatusCode::Good,
      .notification_message = {.sequence_number = 3, .notification_data = {}},
  };
  const auto decoded = RoundTrip(28, response);
  const auto& typed = std::get<RepublishResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  EXPECT_EQ(typed.notification_message.sequence_number, 3u);
  EXPECT_TRUE(typed.notification_message.notification_data.empty());
}

TEST(ServiceCodecTest, DecodeResponseRejectsMalformedExtensionObject) {
  // Raw bytes that cannot be decoded as a valid ExtensionObject wrapper —
  // the dispatcher must refuse rather than misinterpret.
  EXPECT_FALSE(DecodeServiceResponse(std::vector<char>{}).has_value());
  EXPECT_FALSE(
      DecodeServiceResponse(std::vector<char>{0x00, 0x00, 0x00}).has_value());
}

TEST(ServiceCodecTest, DecodeResponseRejectsUnknownTypeId) {
  // Valid ExtensionObject wrapper but type id not in the response dispatch
  // table.
  std::vector<char> body;
  body.push_back(0x01);  // NodeId two-byte encoding
  body.push_back(0x00);
  body.push_back(0x55);  // numeric id 85 — not a response encoding id
  body.push_back(0x55);
  body.push_back(0x01);  // encoding = ByteString
  body.push_back(0x00);  // length = 0
  body.push_back(0x00);
  body.push_back(0x00);
  body.push_back(0x00);
  EXPECT_FALSE(DecodeServiceResponse(body).has_value());
}

// -- RequestHeader.additionalHeader traceparent carrier (OPC UA Part 4 §7.33
// RequestHeader; AdditionalParametersType i=16313 / Default Binary i=17537).
// The decode side is deliberately tolerant: unknown or malformed
// additionalHeader content yields an empty trace_parent, never a decode
// failure.

constexpr std::string_view kTraceParentForTest =
    "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

constexpr std::uint32_t kAdditionalParametersEncodingIdForTest = 17537;

// Encodes the RequestHeader fixed fields followed by a raw additionalHeader
// ExtensionObject with the given type id and ByteString body.
void AppendRequestHeaderWithAdditional(Encoder& encoder,
                                       std::uint32_t additional_type_id,
                                       std::span<const char> additional_body) {
  encoder.Encode(opcua::NodeId{77, 3});
  encoder.Encode(std::int64_t{0});
  encoder.Encode(std::uint32_t{41});
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(std::string_view{"audit-entry"});
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(EncodedExtensionObject{
      additional_type_id,
      std::vector<char>{additional_body.begin(), additional_body.end()}});
}

// A minimal decodable ReadRequest body following the header.
void AppendReadRequestBody(Encoder& encoder) {
  encoder.Encode(0.0);
  encoder.Encode(std::uint32_t{2});
  encoder.Encode(std::int32_t{1});
  encoder.Encode(opcua::NodeId{123, 4});
  encoder.Encode(static_cast<std::uint32_t>(opcua::AttributeId::Value));
  encoder.Encode(std::string_view{});
  encoder.Encode(opcua::QualifiedName{"Default Binary", 0});
}

TEST(ServiceCodecTest, RequestHeaderTraceParentRoundTrip) {
  const ServiceRequestHeader header{
      .authentication_token = opcua::NodeId{77, 3},
      .request_handle = 41,
      .trace_parent = std::string{kTraceParentForTest},
  };
  const auto encoded = EncodeServiceRequest(
      header, RequestBody{ua::ReadRequest{
                  .nodes_to_read = {ua::ReadValueId{
                      .node_id = opcua::NodeId{123, 4},
                      .attribute_id = static_cast<opcua::UInt32>(
                          opcua::AttributeId::Value)}}}});
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->header.request_handle, 41u);
  EXPECT_EQ(decoded->header.trace_parent, kTraceParentForTest);
}

TEST(ServiceCodecTest, RequestHeaderWithoutTraceParentDecodesEmpty) {
  const ServiceRequestHeader header{
      .authentication_token = opcua::NodeId{77, 3},
      .request_handle = 41,
  };
  const auto encoded = EncodeServiceRequest(
      header, RequestBody{ua::ReadRequest{
                  .nodes_to_read = {ua::ReadValueId{
                      .node_id = opcua::NodeId{123, 4},
                      .attribute_id = static_cast<opcua::UInt32>(
                          opcua::AttributeId::Value)}}}});
  ASSERT_TRUE(encoded.has_value());

  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->header.trace_parent.empty());
}

TEST(ServiceCodecTest, UnknownAdditionalHeaderTypeIsSkipped) {
  std::vector<char> additional_body;
  Encoder additional_encoder{additional_body};
  additional_encoder.Encode(std::string_view{"opaque vendor payload"});

  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderWithAdditional(encoder, /*additional_type_id=*/9999,
                                    additional_body);
  AppendReadRequestBody(encoder);

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->header.trace_parent.empty());
}

TEST(ServiceCodecTest, TruncatedAdditionalParametersBodyIsDropped) {
  // Count says one KeyValuePair but no pair bytes follow.
  std::vector<char> additional_body;
  Encoder additional_encoder{additional_body};
  additional_encoder.Encode(std::int32_t{1});

  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderWithAdditional(
      encoder, kAdditionalParametersEncodingIdForTest, additional_body);
  AppendReadRequestBody(encoder);

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->header.trace_parent.empty());
}

TEST(ServiceCodecTest, AdditionalParametersCountBombIsDropped) {
  // A huge parameter count must not fail the request (the header is an
  // optional extension) and must not allocate for ~2e9 entries.
  std::vector<char> additional_body;
  Encoder additional_encoder{additional_body};
  additional_encoder.Encode(std::int32_t{2000000000});

  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderWithAdditional(
      encoder, kAdditionalParametersEncodingIdForTest, additional_body);
  AppendReadRequestBody(encoder);

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->header.trace_parent.empty());
}

TEST(ServiceCodecTest, NonStringTraceParentValueIsDropped) {
  std::vector<char> additional_body;
  Encoder additional_encoder{additional_body};
  additional_encoder.Encode(std::int32_t{1});
  additional_encoder.Encode(opcua::QualifiedName{"traceparent", 0});
  additional_encoder.Encode(opcua::Variant{std::int32_t{42}});

  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderWithAdditional(
      encoder, kAdditionalParametersEncodingIdForTest, additional_body);
  AppendReadRequestBody(encoder);

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->header.trace_parent.empty());
}

TEST(ServiceCodecTest, TraceParentAmongOtherParametersIsFound) {
  std::vector<char> additional_body;
  Encoder additional_encoder{additional_body};
  additional_encoder.Encode(std::int32_t{2});
  additional_encoder.Encode(opcua::QualifiedName{"other", 0});
  additional_encoder.Encode(opcua::Variant{opcua::String{"value"}});
  additional_encoder.Encode(opcua::QualifiedName{"traceparent", 0});
  additional_encoder.Encode(opcua::Variant{opcua::String{kTraceParentForTest}});

  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderWithAdditional(
      encoder, kAdditionalParametersEncodingIdForTest, additional_body);
  AppendReadRequestBody(encoder);

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(kReadRequestEncodingIdForTest, payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->header.trace_parent, kTraceParentForTest);
}

// The bespoke ACKED/UNACKED selection travels the wire as the standard
// `Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean))` where
// clause (OPC UA Part 4 §7.7.3), so it must round-trip through the binary
// codec next to OfType / RelatedTo.
TEST(ServiceCodecTest, EventFilterTypesAndWhereClauseRoundTrip) {
  auto json = BuildEventFilter(
      std::vector<std::vector<std::string>>{{"EventId"}, {"Severity"}});
  auto& object = json.as_object();
  object["_scada"] = "event";
  object["types"] = EventFilter::UNACKED;
  object["of_type"] = boost::json::array{"i=2130"};
  object["child_of"] = boost::json::array{"ns=4;i=17"};

  CreateMonitoredItemsRequest request{
      .subscription_id = 9,
      .items_to_create = {MonitoredItemCreateRequest{
          .item_to_monitor = {.node_id = opcua::NodeId{2253u},
                              .attribute_id = AttributeId::EventNotifier},
          .requested_parameters = {
              .client_handle = 5,
              .filter = MonitoringFilter{std::move(json)}}}}};

  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<CreateMonitoredItemsRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->items_to_create.size(), 1u);
  const auto& filter = typed->items_to_create[0].requested_parameters.filter;
  ASSERT_TRUE(filter.has_value());
  const auto* decoded_json = std::get_if<boost::json::value>(&*filter);
  ASSERT_NE(decoded_json, nullptr);
  const auto& decoded_object = decoded_json->as_object();
  EXPECT_EQ(decoded_object.at("types").to_number<unsigned>(),
            EventFilter::UNACKED);
  ASSERT_TRUE(decoded_object.at("of_type").is_array());
  EXPECT_EQ(decoded_object.at("of_type").as_array().at(0).as_string(),
            "i=2130");
  EXPECT_EQ(decoded_object.at("child_of").as_array().at(0).as_string(),
            "ns=4;i=17");
}

// A foreign client's where clause with operators this server does not
// evaluate must degrade to less server-side filtering, not to a rejected
// CreateMonitoredItems: unknown elements are skipped structurally (every
// ContentFilter operand is an extension object, OPC UA Part 4 §7.7.4) and
// the supported OfType clause still parses.
TEST(ServiceCodecTest, EventFilterSkipsUnsupportedWhereClauseOperators) {
  const auto append_literal = [](Encoder& encoder, const Variant& value) {
    std::vector<char> body;
    Encoder body_encoder{body};
    body_encoder.Encode(value);
    encoder.Encode(EncodedExtensionObject{.type_id = 597 /*LiteralOperand*/,
                                          .body = std::move(body)});
  };

  std::vector<char> filter_body;
  Encoder filter_encoder{filter_body};
  filter_encoder.Encode(std::int32_t{0});  // No select clauses (defaulted).
  filter_encoder.Encode(std::int32_t{2});  // Where clause elements.
  // Unsupported: And(Literal, Literal) — skipped.
  filter_encoder.Encode(std::uint32_t{10} /*And*/);
  filter_encoder.Encode(std::int32_t{2});
  append_literal(filter_encoder, Variant{true});
  append_literal(filter_encoder, Variant{false});
  // Supported: OfType(Literal NodeId) — still parsed. OfType is the spec
  // ContentFilter operator 14 (the hand-written codec used a wrong 11).
  filter_encoder.Encode(std::uint32_t{14} /*OfType*/);
  filter_encoder.Encode(std::int32_t{1});
  append_literal(filter_encoder, Variant{opcua::NodeId{501u}});

  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderForTest(encoder, opcua::NodeId{77, 3}, 41, "audit");
  encoder.Encode(std::uint32_t{9});  // subscription_id
  encoder.Encode(std::uint32_t{2});  // TimestampsToReturn::Both
  encoder.Encode(std::int32_t{1});   // item count
  encoder.Encode(opcua::NodeId{2253u});
  encoder.Encode(static_cast<std::uint32_t>(AttributeId::EventNotifier));
  encoder.Encode(std::string_view{});  // index range
  encoder.Encode(QualifiedName{});     // data encoding
  encoder.Encode(std::uint32_t{2});    // MonitoringMode::Reporting
  encoder.Encode(std::uint32_t{5});    // client handle
  encoder.Encode(0.0);                 // sampling interval
  encoder.Encode(EncodedExtensionObject{.type_id = 727 /*EventFilter*/,
                                        .body = std::move(filter_body)});
  encoder.Encode(std::uint32_t{1});  // queue size
  encoder.Encode(true);              // discard oldest

  const auto decoded = DecodeServiceRequest(
      WrapMessageForTest(751 /*CreateMonitoredItemsRequest*/, payload));

  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<CreateMonitoredItemsRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->items_to_create.size(), 1u);
  const auto& filter = typed->items_to_create[0].requested_parameters.filter;
  ASSERT_TRUE(filter.has_value());
  const auto* decoded_json = std::get_if<boost::json::value>(&*filter);
  ASSERT_NE(decoded_json, nullptr);
  const auto& decoded_object = decoded_json->as_object();
  ASSERT_TRUE(decoded_object.at("of_type").is_array());
  EXPECT_EQ(decoded_object.at("of_type").as_array().at(0).as_string(), "i=501");
}

}  // namespace
}  // namespace opcua::binary
