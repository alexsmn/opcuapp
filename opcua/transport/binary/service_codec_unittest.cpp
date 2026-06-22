#include "opcua/transport/binary/service_codec.h"

#include "opcua/transport/binary/codec_utils.h"

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

constexpr std::uint32_t kReadRequestEncodingIdForTest = 629;
constexpr std::uint32_t kWriteRequestEncodingIdForTest = 671;
constexpr std::uint32_t kBrowseResponseEncodingIdForTest = 528;

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
  const auto& request = std::get<ReadRequest>(decoded->body);
  ASSERT_EQ(request.inputs.size(), 1u);
  EXPECT_EQ(request.inputs[0].node_id, (opcua::NodeId{123, 4}));
  EXPECT_EQ(request.inputs[0].attribute_id, opcua::AttributeId::Value);
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

TEST(ServiceCodecTest, DecodeWriteRequestRejectsNonEmptyIndexRange) {
  std::vector<char> payload;
  Encoder encoder{payload};
  AppendRequestHeaderForTest(encoder, opcua::NodeId{77, 3}, 42, "audit-entry");
  encoder.Encode(std::int32_t{1});
  encoder.Encode(opcua::NodeId{123, 4});
  encoder.Encode(static_cast<std::uint32_t>(opcua::AttributeId::Value));
  encoder.Encode(std::string_view{"0:1"});
  encoder.Encode(std::uint8_t{0x01});
  encoder.Encode(opcua::Variant{std::int32_t{7}});

  EXPECT_FALSE(DecodeServiceRequest(
      WrapMessageForTest(kWriteRequestEncodingIdForTest, payload)));
}

TEST(ServiceCodecTest, DecodeBrowseResponseSkipsBrowseNameAndDisplayName) {
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
  const auto& response = std::get<BrowseResponse>(decoded->body);
  ASSERT_EQ(response.results.size(), 1u);
  ASSERT_EQ(response.results[0].references.size(), 1u);
  EXPECT_EQ(response.results[0].references[0].reference_type_id,
            opcua::NodeId{35});
  EXPECT_TRUE(response.results[0].references[0].forward);
  EXPECT_EQ(response.results[0].references[0].node_id, (opcua::NodeId{456, 4}));
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
      .revised_timeout = opcua::base::TimeDelta::FromMilliseconds(60000),
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
      .requested_timeout = opcua::base::TimeDelta::FromSeconds(120),
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
  HistoryUpdateRequest request{
      .details = {.node_id = opcua::NodeId{opcua::String{"Tag"}, 2},
                  .perform_insert_replace = PerformUpdateType::Replace,
                  .values = {value}}};
  const auto encoded = EncodeServiceRequest({}, RequestBody{request});
  ASSERT_TRUE(encoded.has_value());
  const auto decoded = DecodeServiceRequest(*encoded);
  ASSERT_TRUE(decoded.has_value());
  const auto* typed = std::get_if<HistoryUpdateRequest>(&decoded->body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->details.node_id, request.details.node_id);
  EXPECT_EQ(typed->details.perform_insert_replace, PerformUpdateType::Replace);
  ASSERT_EQ(typed->details.values.size(), 1u);
  EXPECT_EQ(typed->details.values[0].value, value.value);
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
  EXPECT_TRUE(std::holds_alternative<ActivateSessionRequest>(decoded->body));
}

TEST(ServiceCodecTest, AddNodesResponseRoundTrip) {
  AddNodesResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::AddNodesResult{
          .status_code = opcua::StatusCode::Good,
          .added_node_id = opcua::NodeId{101, 6},
      }},
  };

  const auto decoded = RoundTrip(18, response);

  const auto& typed = std::get<AddNodesResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_EQ(typed.results[0].status_code, opcua::StatusCode::Good);
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
  ReadResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::DataValue{
                      opcua::Variant{std::int32_t{42}}, {}, {}, {}},
                  bad_value},
  };
  const auto decoded = RoundTrip(11, response);
  const auto& typed = std::get<ReadResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_EQ(typed.results[0].value, response.results[0].value);
  EXPECT_TRUE(opcua::IsGood(typed.results[0].status_code));
  EXPECT_EQ(typed.results[1].value, response.results[1].value);
  EXPECT_EQ(typed.results[1].status_code, opcua::StatusCode::Bad);
}

TEST(ServiceCodecTest, WriteResponseRoundTrip) {
  WriteResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good,
                  opcua::StatusCode::Bad_WrongAttributeId},
  };
  const auto decoded = RoundTrip(12, response);
  const auto& typed = std::get<WriteResponse>(decoded.body);
  EXPECT_TRUE(typed.status.good());
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_EQ(typed.results[0], opcua::StatusCode::Good);
  EXPECT_EQ(typed.results[1], opcua::StatusCode::Bad_WrongAttributeId);
}

TEST(ServiceCodecTest, BrowseResponseRoundTrip) {
  const opcua::ByteString opaque_id{'o', 'p', 'a', 'q', 'u', 'e'};
  BrowseResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::BrowseResult{
                      .status_code = opcua::StatusCode::Good,
                      .continuation_point = opcua::ByteString{'c', 'p'},
                      .references =
                          {opcua::ReferenceDescription{
                               .reference_type_id = opcua::NodeId{35},
                               .forward = true,
                               .node_id = opcua::NodeId{100},
                               .node_class = opcua::NodeClass::Variable,
                           },
                           opcua::ReferenceDescription{
                               .reference_type_id = opcua::NodeId{35},
                               .forward = true,
                               .node_id =
                                   opcua::NodeId{opcua::String{"File.txt"}, 7},
                           },
                           opcua::ReferenceDescription{
                               .reference_type_id = opcua::NodeId{35},
                               .forward = true,
                               .node_id = opcua::NodeId{opaque_id, 8},
                           },
                           opcua::ReferenceDescription{
                               .reference_type_id = opcua::NodeId{35},
                               .forward = true,
                               .node_id = opcua::NodeId{200},
                               .node_class = opcua::NodeClass::Object,
                               .browse_name = opcua::QualifiedName{"Widget", 2},
                               .display_name = opcua::LocalizedText{u"Widget"},
                               .type_definition = opcua::NodeId{58},
                           }},
                  },
                  opcua::BrowseResult{
                      .status_code = opcua::StatusCode::Bad_NothingToDo,
                  }},
  };
  const auto decoded = RoundTrip(13, response);
  const auto& typed = std::get<BrowseResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_TRUE(opcua::IsGood(typed.results[0].status_code));
  EXPECT_EQ(typed.results[0].continuation_point,
            response.results[0].continuation_point);
  ASSERT_EQ(typed.results[0].references.size(), 4u);
  EXPECT_EQ(typed.results[0].references[0].reference_type_id,
            opcua::NodeId{35});
  EXPECT_TRUE(typed.results[0].references[0].forward);
  EXPECT_EQ(typed.results[0].references[0].node_id, opcua::NodeId{100});
  EXPECT_EQ(typed.results[0].references[0].node_class,
            opcua::NodeClass::Variable);
  EXPECT_EQ(typed.results[0].references[1].node_id,
            (opcua::NodeId{opcua::String{"File.txt"}, 7}));
  EXPECT_EQ(typed.results[0].references[2].node_id,
            (opcua::NodeId{opaque_id, 8}));
  // BrowseName / DisplayName / TypeDefinition survive the round trip.
  EXPECT_EQ(typed.results[0].references[3].browse_name,
            (opcua::QualifiedName{"Widget", 2}));
  EXPECT_EQ(typed.results[0].references[3].display_name,
            opcua::LocalizedText{u"Widget"});
  EXPECT_EQ(typed.results[0].references[3].type_definition, opcua::NodeId{58});
  EXPECT_EQ(typed.results[1].status_code, opcua::StatusCode::Bad_NothingToDo);
  EXPECT_TRUE(typed.results[1].references.empty());
}

TEST(ServiceCodecTest, BrowseNextResponseRoundTrip) {
  BrowseNextResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::BrowseResult{.status_code = opcua::StatusCode::Good}},
  };
  const auto decoded = RoundTrip(14, response);
  const auto& typed = std::get<BrowseNextResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(opcua::IsGood(typed.results[0].status_code));
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
  CallResponse response;
  response.results.push_back(
      {.status = opcua::StatusCode::Good,
       .input_argument_results = {opcua::StatusCode::Good},
       .output_arguments = {opcua::Variant{std::int32_t{100}},
                            opcua::Variant{std::int32_t{200}}}});
  const auto decoded = RoundTrip(16, response);
  const auto& typed = std::get<CallResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_TRUE(typed.results[0].status.good());
  ASSERT_EQ(typed.results[0].input_argument_results.size(), 1u);
  EXPECT_EQ(typed.results[0].input_argument_results[0],
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
  DeleteSubscriptionsResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good, opcua::StatusCode::Bad},
  };
  const auto decoded = RoundTrip(20, response);
  const auto& typed = std::get<DeleteSubscriptionsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_EQ(typed.results[0], opcua::StatusCode::Good);
  EXPECT_EQ(typed.results[1], opcua::StatusCode::Bad);
}

TEST(ServiceCodecTest, SetPublishingModeResponseRoundTrip) {
  SetPublishingModeResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good},
  };
  const auto decoded = RoundTrip(21, response);
  const auto& typed = std::get<SetPublishingModeResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_EQ(typed.results[0], opcua::StatusCode::Good);
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
  DeleteMonitoredItemsResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good,
                  opcua::StatusCode::Bad_MonitoredItemIdInvalid},
  };
  const auto decoded = RoundTrip(24, response);
  const auto& typed = std::get<DeleteMonitoredItemsResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 2u);
  EXPECT_EQ(typed.results[0], opcua::StatusCode::Good);
  EXPECT_EQ(typed.results[1], opcua::StatusCode::Bad_MonitoredItemIdInvalid);
}

TEST(ServiceCodecTest, SetMonitoringModeResponseRoundTrip) {
  SetMonitoringModeResponse response{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good},
  };
  const auto decoded = RoundTrip(25, response);
  const auto& typed = std::get<SetMonitoringModeResponse>(decoded.body);
  ASSERT_EQ(typed.results.size(), 1u);
  EXPECT_EQ(typed.results[0], opcua::StatusCode::Good);
}

TEST(ServiceCodecTest, PublishResponseRoundTripDataChange) {
  PublishResponse response{
      .status = opcua::StatusCode::Good,
      .subscription_id = 42,
      .results = {opcua::StatusCode::Good},
      .more_notifications = false,
      .notification_message =
          {.sequence_number = 7,
           .publish_time = opcua::base::Time{},
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
               .status = opcua::StatusCode::Bad_WrongSubscriptionId}}},
  };
  const auto decoded = RoundTrip(27, response);
  const auto& typed = std::get<PublishResponse>(decoded.body);
  ASSERT_EQ(typed.notification_message.notification_data.size(), 1u);
  const auto* change = std::get_if<StatusChangeNotification>(
      &typed.notification_message.notification_data[0]);
  ASSERT_NE(change, nullptr);
  EXPECT_EQ(change->status, opcua::StatusCode::Bad_WrongSubscriptionId);
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

}  // namespace
}  // namespace opcua::binary
