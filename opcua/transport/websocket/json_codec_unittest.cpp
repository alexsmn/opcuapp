#include "opcua/transport/websocket/json_codec.h"
#include "opcua/events/event_filter.h"
#include "opcua/services/history_conversion.h"

#include "opcua/base/time_utils.h"
#include "opcua/events/event.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"
#include "opcua/ua/ua_service_header.h"

#include <boost/json.hpp>
#include <boost/json/serialize.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <type_traits>

using namespace testing;

namespace opcua::ws {
namespace {

opcua::NodeId NumericNode(opcua::NumericId id, opcua::NamespaceIndex ns = 2) {
  return {id, ns};
}

opcua::DateTime ParseTime(std::string_view value) {
  opcua::DateTime result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

// Pin the spec wire shape (Part 6 §5.4.2.10–17) for the primitive codecs by
// asserting on the literal JSON the codec emits when wrapping each primitive
// inside a Read response (the smallest envelope that carries DataValue and
// Variant). Future regressions surface here without needing the full e2e.
TEST(JsonCodecTest, ReadResponseWireShapeMatchesSpec) {
  ua::ReadResponse read{
      .results = {opcua::DataValue{opcua::Variant{opcua::Int32{42}},
                                   opcua::Qualifier{opcua::Qualifier::MANUAL},
                                   ParseTime("2026-04-19 10:00:00"),
                                   ParseTime("2026-04-19 10:00:01")}}};
  // Force a non-default status_code so it lands on the wire.
  read.results[0].status_code = opcua::StatusCode::Bad_NoCommunication;

  const auto encoded = EncodeJson(ServiceResponse{read});
  ASSERT_TRUE(encoded.is_object());
  const auto& body = encoded.as_object().at("body").as_object();
  // The generated response envelope carries a ResponseHeader, not a bare
  // top-level Status, plus the Results array.
  ASSERT_TRUE(body.contains("ResponseHeader"));
  ASSERT_TRUE(body.contains("Results"));
  const auto& dv = body.at("Results").as_array().at(0).as_object();

  // Part 6 §5.4.2.18: the generated DataValue inlines the Variant (UaType +
  // Value) and carries StatusCode + Source/ServerTimestamp; the scada-specific
  // Qualifier never appears on the wire.
  EXPECT_TRUE(dv.contains("UaType"));
  EXPECT_EQ(dv.at("UaType").to_number<int>(), 6);  // Int32
  EXPECT_EQ(dv.at("Value").to_number<int>(), 42);
  EXPECT_TRUE(dv.contains("StatusCode"));
  EXPECT_FALSE(dv.contains("Qualifier"));
  EXPECT_TRUE(dv.contains("SourceTimestamp"));
  EXPECT_TRUE(dv.contains("ServerTimestamp"));
  EXPECT_EQ(dv.at("SourceTimestamp").as_string(), "2026-04-19T10:00:00Z");
  EXPECT_EQ(dv.at("ServerTimestamp").as_string(), "2026-04-19T10:00:01Z");

  // Round-trip preserves Variant value, Status and timestamps. Qualifier
  // is intentionally dropped (no spec field), so the decoded qualifier
  // returns to default — assert per-field rather than via vector equality.
  const auto decoded =
      std::get<ua::ReadResponse>(*DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  EXPECT_TRUE(decoded.results[0].value == read.results[0].value);
  EXPECT_EQ(decoded.results[0].status_code, read.results[0].status_code);
  EXPECT_EQ(decoded.results[0].source_timestamp,
            read.results[0].source_timestamp);
  EXPECT_EQ(decoded.results[0].server_timestamp,
            read.results[0].server_timestamp);
  EXPECT_EQ(decoded.results[0].qualifier.raw(), 0u);
}

TEST(JsonCodecTest, BrowseResponseWireShapeMatchesSpec) {
  ua::BrowseResponse browse{
      .results = {
          {.status_code = Status{opcua::StatusCode::Good},
           .references = {{.reference_type_id = NumericNode(301),
                           .is_forward = false,
                           .node_id = opcua::ExpandedNodeId{NumericNode(302)},
                           .node_class = ua::NodeClass::Variable}}}}};

  const auto encoded = EncodeJson(ServiceResponse{browse});
  const auto& result = encoded.as_object()
                           .at("body")
                           .as_object()
                           .at("Results")
                           .as_array()
                           .at(0)
                           .as_object();
  const auto& reference = result.at("References").as_array().at(0).as_object();
  EXPECT_FALSE(result.contains("ContinuationPoint"));

  // §5.4.2.10/.11: NodeId / ReferenceTypeId on a Reference are JSON strings.
  EXPECT_TRUE(reference.at("NodeId").is_string());
  EXPECT_EQ(reference.at("NodeId").as_string(), "ns=2;i=302");
  EXPECT_TRUE(reference.at("ReferenceTypeId").is_string());
  EXPECT_EQ(reference.at("ReferenceTypeId").as_string(), "ns=2;i=301");
  // OPC UA Part 4 Browse resultMask bit 2 is NodeClass, and
  // ReferenceDescription.nodeClass is the target Node's NodeClass:
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.9.2.2
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.29
  EXPECT_EQ(reference.at("NodeClass").to_number<int>(), 2);

  // Roundtrip preserves the Reference's NodeId, forward flag, and NodeClass.
  const auto decoded =
      std::get<ua::BrowseResponse>(*DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  ASSERT_EQ(decoded.results[0].references.size(), 1u);
  EXPECT_EQ(decoded.results[0].references[0].node_id.node_id(),
            NumericNode(302));
  EXPECT_FALSE(decoded.results[0].references[0].is_forward);
  EXPECT_EQ(decoded.results[0].references[0].node_class,
            ua::NodeClass::Variable);
}

TEST(JsonCodecTest, TranslateBrowsePathsWireShapeMatchesSpec) {
  ua::TranslateBrowsePathsToNodeIdsResponse translate;
  translate.results = {
      {.status_code = opcua::Status{opcua::StatusCode::Good},
       .targets = {
           {.target_id = opcua::ExpandedNodeId{NumericNode(303), "urn:test", 2},
            .remaining_path_index = 1}}}};

  const auto encoded = EncodeJson(ServiceResponse{translate});
  const auto& target = encoded.as_object()
                           .at("body")
                           .as_object()
                           .at("Results")
                           .as_array()
                           .at(0)
                           .as_object()
                           .at("Targets")
                           .as_array()
                           .at(0)
                           .as_object();

  // §5.4.2.11: ExpandedNodeId is a JSON string with optional `svr=` and
  // `nsu=` prefixes followed by the NodeId text.
  EXPECT_TRUE(target.at("TargetId").is_string());
  EXPECT_EQ(target.at("TargetId").as_string(), "svr=2;nsu=urn:test;ns=2;i=303");

  const auto decoded = std::get<ua::TranslateBrowsePathsToNodeIdsResponse>(
      *DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  ASSERT_EQ(decoded.results[0].targets.size(), 1u);
  EXPECT_EQ(decoded.results[0].targets[0].target_id.node_id(),
            NumericNode(303));
  EXPECT_EQ(decoded.results[0].targets[0].target_id.namespace_uri(),
            "urn:test");
  EXPECT_EQ(decoded.results[0].targets[0].target_id.server_index(), 2u);
}

TEST(JsonCodecTest, EmptyVariantSerialisesAsJsonNull) {
  // §5.4.2.16: a null Variant is encoded as the JSON literal `null`,
  // not as `{ Type: 0, ... }`. Verify via a Read response carrying an
  // empty DataValue (which still emits an empty body object since all
  // fields are at their defaults).
  ua::ReadResponse read{.results = {opcua::DataValue{}}};
  const auto encoded = EncodeJson(ServiceResponse{read});
  const auto& dv = encoded.as_object()
                       .at("body")
                       .as_object()
                       .at("Results")
                       .as_array()
                       .at(0)
                       .as_object();
  // No UaType/Value, no StatusCode (Good is default), no timestamps — all
  // elided.
  EXPECT_TRUE(dv.empty());
}

namespace {

// Returns a reference INTO `encoded`, so the caller must keep it alive — pass
// a named value, never the result of EncodeJson directly. Binding to a
// temporary here segfaults rather than failing an assertion.
const boost::json::object& ReadResultValue(const boost::json::value& encoded,
                                           std::size_t index = 0) {
  return encoded.as_object()
      .at("body")
      .as_object()
      .at("Results")
      .as_array()
      .at(index)
      .as_object()
      .at("Value")
      .as_object();
}

}  // namespace

// An attribute whose VALUE is a structure reaches a JSON peer as a JSON body,
// not as base64.
//
// `opcua_bridge` wraps structures in BINARY-bodied ExtensionObjects, which are
// conformant on this transport (Part 6 §5.4.2.16: Encoding 1 = ByteString) but
// leave a JSON-only peer — the web client — needing a binary decoder for the
// structure inside. That is how UserManagement.Users (Part 18 §5.2.4) is
// served, so without this the web could not read the account list at all.
TEST(JsonCodecTest, StructuredVariantReachesJsonPeersAsAJsonBody) {
  ua::UserManagementDataType user{.user_name = "ivanov"};
  opcua::DataValue data_value;
  data_value.value = opcua::Variant{ua::ToExtensionObject(user)};
  ua::ReadResponse read{.results = {data_value}};

  const auto encoded = EncodeJson(ServiceResponse{read});
  const auto& body = ReadResultValue(encoded);

  // Encoding is ABSENT for a JSON body (1 would mean base64), the type id is
  // the DefaultJson one, and the structure is readable without a binary
  // decoder.
  EXPECT_EQ(body.at("UaTypeId").as_string(), "i=24300");
  EXPECT_FALSE(body.contains("UaEncoding"));
  EXPECT_EQ(body.at("UaBody").as_object().at("UserName").as_string(), "ivanov");
}

// The array form is what Users actually is: one Read returns every account.
TEST(JsonCodecTest, StructuredVariantArrayTranscodesEveryEntry) {
  std::vector<opcua::ExtensionObject> users{
      ua::ToExtensionObject(ua::UserManagementDataType{.user_name = "root"}),
      ua::ToExtensionObject(ua::UserManagementDataType{.user_name = "ivanov"})};
  opcua::DataValue data_value;
  data_value.value = opcua::Variant{std::move(users)};
  ua::ReadResponse read{.results = {data_value}};

  const auto encoded = EncodeJson(ServiceResponse{read});

  // An array Variant encodes `Value` as a JSON array of ExtensionObjects, one
  // per account — this is the shape a single Read of Users returns.
  const auto& entries = encoded.as_object()
                            .at("body")
                            .as_object()
                            .at("Results")
                            .as_array()
                            .at(0)
                            .as_object()
                            .at("Value")
                            .as_array();
  ASSERT_EQ(entries.size(), 2u);
  for (const auto& entry : entries) {
    EXPECT_FALSE(entry.as_object().contains("UaEncoding"));
  }
  EXPECT_EQ(entries.at(0)
                .as_object()
                .at("UaBody")
                .as_object()
                .at("UserName")
                .as_string(),
            "root");
  EXPECT_EQ(entries.at(1)
                .as_object()
                .at("UaBody")
                .as_object()
                .at("UserName")
                .as_string(),
            "ivanov");
}

// A structure this build does not recognise must still cross exactly as
// before — as base64 — rather than being dropped by the transcode attempt.
TEST(JsonCodecTest, AnUnrecognisedStructuredBodyStillCrossesAsBase64) {
  opcua::ExtensionObject unknown{
      opcua::ExpandedNodeId{opcua::NodeId{/*numeric_id=*/60000, 0}},
      std::any{std::vector<char>{1, 2, 3}}};
  opcua::DataValue data_value;
  data_value.value = opcua::Variant{unknown};
  ua::ReadResponse read{.results = {data_value}};

  const auto encoded = EncodeJson(ServiceResponse{read});
  const auto& body = ReadResultValue(encoded);

  EXPECT_EQ(body.at("UaEncoding").as_int64(), 1);
  EXPECT_TRUE(body.at("UaBody").is_string());
}

TEST(JsonCodecTest, LocalizedTextVariantCarriesLocale) {
  // §5.4.2.14: LocalizedText is `{ Locale?, Text? }`. Verify the Locale field
  // is emitted and survives the round trip on the Variant path.
  ua::ReadResponse read{
      .results = {opcua::DataValue{
          opcua::Variant{opcua::LocalizedText{"ru", u"Насос"}}, {}, {}, {}}}};
  const auto encoded = EncodeJson(ServiceResponse{read});
  // The generated DataValue inlines the Variant: the LocalizedText payload is
  // the DataValue's own Value member.
  const auto& body = encoded.as_object()
                         .at("body")
                         .as_object()
                         .at("Results")
                         .as_array()
                         .at(0)
                         .as_object()
                         .at("Value")
                         .as_object();
  EXPECT_EQ(body.at("Locale").as_string(), "ru");

  const auto decoded =
      std::get<ua::ReadResponse>(*DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  EXPECT_EQ(decoded.results[0].value,
            (opcua::Variant{opcua::LocalizedText{"ru", u"Насос"}}));
}

TEST(JsonCodecTest, RoundTripsPhase0Requests) {
  ua::ReadRequest read{
      .nodes_to_read = {{.node_id = NumericNode(1),
                         .attribute_id = static_cast<opcua::UInt32>(
                             opcua::AttributeId::DisplayName)}}};
  ua::WriteRequest write{
      .nodes_to_write = {
          {.node_id = NumericNode(2),
           .attribute_id =
               static_cast<opcua::UInt32>(opcua::AttributeId::Value),
           .value = opcua::DataValue{
               opcua::Variant{std::vector<opcua::UInt32>{4, 5}}, {}, {}, {}}}}};
  ua::BrowseRequest browse{
      .requested_max_references_per_node = 7,
      .nodes_to_browse = {{.node_id = NumericNode(3),
                           .browse_direction = ua::BrowseDirection::Inverse,
                           .reference_type_id = NumericNode(31),
                           .include_subtypes = false}}};
  ua::BrowseNextRequest browse_next{
      .release_continuation_points = true,
      .continuation_points = {{'a', 'b'}, {'x', 'y', 'z'}}};
  ua::TranslateBrowsePathsToNodeIdsRequest translate{
      .browse_paths = {
          {.starting_node = NumericNode(4),
           .relative_path = {.elements = {{.reference_type_id = NumericNode(41),
                                           .is_inverse = true,
                                           .include_subtypes = false,
                                           .target_name = {"Child", 6}}}}}}};

  const auto decoded_read = std::get<ua::ReadRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{read})));
  ASSERT_EQ(decoded_read.nodes_to_read.size(), 1u);
  EXPECT_EQ(decoded_read.nodes_to_read[0].node_id,
            read.nodes_to_read[0].node_id);
  EXPECT_EQ(decoded_read.nodes_to_read[0].attribute_id,
            read.nodes_to_read[0].attribute_id);

  const auto decoded_write = std::get<ua::WriteRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{write})));
  ASSERT_EQ(decoded_write.nodes_to_write.size(), 1u);
  EXPECT_EQ(decoded_write.nodes_to_write[0].node_id,
            write.nodes_to_write[0].node_id);
  EXPECT_EQ(decoded_write.nodes_to_write[0].attribute_id,
            write.nodes_to_write[0].attribute_id);
  EXPECT_EQ(decoded_write.nodes_to_write[0].value.value,
            write.nodes_to_write[0].value.value);

  const auto decoded_browse = std::get<ua::BrowseRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{browse})));
  EXPECT_EQ(decoded_browse.requested_max_references_per_node,
            browse.requested_max_references_per_node);
  ASSERT_EQ(decoded_browse.nodes_to_browse.size(), 1u);
  EXPECT_EQ(decoded_browse.nodes_to_browse[0].node_id,
            browse.nodes_to_browse[0].node_id);
  EXPECT_EQ(decoded_browse.nodes_to_browse[0].browse_direction,
            browse.nodes_to_browse[0].browse_direction);
  EXPECT_EQ(decoded_browse.nodes_to_browse[0].reference_type_id,
            browse.nodes_to_browse[0].reference_type_id);

  const auto decoded_browse_next = std::get<ua::BrowseNextRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{browse_next})));
  EXPECT_TRUE(decoded_browse_next.release_continuation_points);
  EXPECT_EQ(decoded_browse_next.continuation_points,
            browse_next.continuation_points);

  const auto decoded_translate =
      std::get<ua::TranslateBrowsePathsToNodeIdsRequest>(
          *DecodeServiceRequest(EncodeJson(ServiceRequest{translate})));
  ASSERT_EQ(decoded_translate.browse_paths.size(), 1u);
  EXPECT_EQ(decoded_translate.browse_paths[0].starting_node, NumericNode(4));
  ASSERT_EQ(decoded_translate.browse_paths[0].relative_path.elements.size(),
            1u);
  const auto& element =
      decoded_translate.browse_paths[0].relative_path.elements[0];
  EXPECT_EQ(element.reference_type_id, NumericNode(41));
  EXPECT_TRUE(element.is_inverse);
  EXPECT_FALSE(element.include_subtypes);
  EXPECT_EQ(element.target_name, opcua::QualifiedName("Child", 6));
}

TEST(JsonCodecTest, RoundTripsCanonicalEnvelopeTypes) {
  const RequestMessage request{
      .request_handle = 91,
      .body = ua::ReadRequest{
          .nodes_to_read = {{.node_id = NumericNode(9),
                             .attribute_id = static_cast<opcua::UInt32>(
                                 opcua::AttributeId::BrowseName)}}}};
  const auto decoded_request = *DecodeRequestMessage(EncodeJson(request));
  EXPECT_EQ(decoded_request.request_handle, request.request_handle);
  const auto* decoded_read =
      std::get_if<ua::ReadRequest>(&decoded_request.body);
  ASSERT_NE(decoded_read, nullptr);
  ASSERT_EQ(decoded_read->nodes_to_read.size(), 1u);
  EXPECT_EQ(decoded_read->nodes_to_read[0].node_id,
            std::get<ua::ReadRequest>(request.body).nodes_to_read[0].node_id);

  const ResponseMessage response{
      .request_handle = 92,
      .body = ServiceFault{.status = opcua::StatusCode::Bad_TypeMismatch}};
  const auto decoded_response = *DecodeResponseMessage(EncodeJson(response));
  EXPECT_EQ(decoded_response.request_handle, response.request_handle);
  const auto* decoded_fault = std::get_if<ServiceFault>(&decoded_response.body);
  ASSERT_NE(decoded_fault, nullptr);
  EXPECT_EQ(decoded_fault->status.code(), opcua::StatusCode::Bad_TypeMismatch);
}

TEST(JsonCodecTest, RequestWireShapeUsesSpecFieldNames) {
  const auto read_json = EncodeJson(ServiceRequest{ua::ReadRequest{
      .nodes_to_read = {{.node_id = NumericNode(1),
                         .attribute_id = static_cast<opcua::UInt32>(
                             opcua::AttributeId::Value)}}}});
  const auto write_json = EncodeJson(ServiceRequest{ua::WriteRequest{
      .nodes_to_write = {{.node_id = NumericNode(2),
                          .attribute_id = static_cast<opcua::UInt32>(
                              opcua::AttributeId::Value),
                          .value = opcua::DataValue{
                              opcua::Variant{opcua::Int32{7}}, {}, {}, {}}}}}});
  const auto browse_json = EncodeJson(ServiceRequest{ua::BrowseRequest{
      .requested_max_references_per_node = 5,
      .nodes_to_browse = {{.node_id = NumericNode(3),
                           .browse_direction = ua::BrowseDirection::Forward,
                           .reference_type_id = NumericNode(31),
                           .include_subtypes = true}}}});
  const auto translate_json =
      EncodeJson(ServiceRequest{ua::TranslateBrowsePathsToNodeIdsRequest{
          .browse_paths = {
              {.starting_node = NumericNode(4),
               .relative_path = {
                   .elements = {{.reference_type_id = NumericNode(41),
                                 .is_inverse = false,
                                 .include_subtypes = true,
                                 .target_name = {"Child", 1}}}}}}}});

  const auto& read_body = read_json.as_object().at("body").as_object();
  EXPECT_TRUE(read_body.contains("NodesToRead"));
  EXPECT_FALSE(read_body.contains("Inputs"));

  const auto& write_body = write_json.as_object().at("body").as_object();
  EXPECT_TRUE(write_body.contains("NodesToWrite"));
  EXPECT_FALSE(write_body.contains("Inputs"));

  const auto& browse_body = browse_json.as_object().at("body").as_object();
  EXPECT_TRUE(browse_body.contains("NodesToBrowse"));
  EXPECT_FALSE(browse_body.contains("Inputs"));

  const auto& translate_body =
      translate_json.as_object().at("body").as_object();
  EXPECT_TRUE(translate_body.contains("BrowsePaths"));
  EXPECT_FALSE(translate_body.contains("Inputs"));
}

TEST(JsonCodecTest, DecodeWriteRequestParsesConformantDataValue) {
  // The spec-conformant WriteValue.Value is a DataValue with the Variant
  // inlined as {UaType, Value} (Part 6 §5.4.2.18), not a nested {Type, Body}
  // wrapper. UaType 12 = String.
  const auto request = *DecodeServiceRequest(boost::json::parse(R"json(
{
  "service": "Write",
  "body": {
    "NodesToWrite": [
      {
        "NodeId": "ns=2;s=UserProfile",
        "AttributeId": 13,
        "Value": {
          "UaType": 12,
          "Value": "{\"version\":1,\"favorites\":[\"ns=2;i=1001\"]}"
        }
      }
    ]
  }
}
)json"));

  const auto* write = std::get_if<ua::WriteRequest>(&request);
  ASSERT_NE(write, nullptr);
  ASSERT_EQ(write->nodes_to_write.size(), 1u);
  EXPECT_EQ(write->nodes_to_write[0].node_id,
            opcua::NodeId::FromString("ns=2;s=UserProfile"));
  EXPECT_EQ(write->nodes_to_write[0].attribute_id,
            static_cast<opcua::UInt32>(opcua::AttributeId::Value));
  EXPECT_EQ(write->nodes_to_write[0].value.value,
            opcua::Variant{opcua::String{
                "{\"version\":1,\"favorites\":[\"ns=2;i=1001\"]}"}});
}

TEST(JsonCodecTest, RoundTripsSessionRequestMessages) {
  RequestMessage create{
      .request_handle = 11,
      .body = CreateSessionRequest{.requested_timeout =
                                       opcua::Duration::FromSeconds(45)}};
  // The conformant ActivateSession binds the session by the authentication
  // token in the RequestHeader; there is no session_id / delete_existing on the
  // wire, and the credentials travel inside the UserIdentityToken.
  RequestMessage activate{.request_handle = 12,
                          .body = ActivateSessionRequest{
                              .authentication_token = NumericNode(21, 3),
                              .user_name = opcua::LocalizedText{u"operator"},
                              .password = opcua::LocalizedText{u"secret"},
                              .allow_anonymous = false,
                          }};
  RequestMessage close{
      .request_handle = 13,
      .body = CloseSessionRequest{.authentication_token = NumericNode(23, 3)}};

  const auto decoded_create = *DecodeRequestMessage(EncodeJson(create));
  EXPECT_EQ(decoded_create.request_handle, create.request_handle);
  const auto* create_body =
      std::get_if<CreateSessionRequest>(&decoded_create.body);
  ASSERT_NE(create_body, nullptr);
  EXPECT_EQ(create_body->requested_timeout, opcua::Duration::FromSeconds(45));

  const auto decoded_activate = *DecodeRequestMessage(EncodeJson(activate));
  EXPECT_EQ(decoded_activate.request_handle, activate.request_handle);
  const auto* activate_body =
      std::get_if<ActivateSessionRequest>(&decoded_activate.body);
  ASSERT_NE(activate_body, nullptr);
  EXPECT_EQ(activate_body->authentication_token, NumericNode(21, 3));
  ASSERT_TRUE(activate_body->user_name.has_value());
  ASSERT_TRUE(activate_body->password.has_value());
  EXPECT_EQ(*activate_body->user_name, opcua::LocalizedText{u"operator"});
  EXPECT_EQ(*activate_body->password, opcua::LocalizedText{u"secret"});
  EXPECT_FALSE(activate_body->allow_anonymous);

  const auto decoded_close = *DecodeRequestMessage(EncodeJson(close));
  EXPECT_EQ(decoded_close.request_handle, close.request_handle);
  const auto* close_body =
      std::get_if<CloseSessionRequest>(&decoded_close.body);
  ASSERT_NE(close_body, nullptr);
  EXPECT_EQ(close_body->authentication_token, NumericNode(23, 3));
}

TEST(JsonCodecTest, EncodesConformantPascalCaseSessionMessageFields) {
  const auto create_json = EncodeJson(RequestMessage{
      .request_handle = 11,
      .body = CreateSessionRequest{.requested_timeout =
                                       opcua::Duration::FromSeconds(45)}});
  const auto create_text = boost::json::serialize(create_json);
  // Spec field names are PascalCase verbatim (Part 6 §5.4).
  EXPECT_NE(create_text.find("\"RequestedSessionTimeout\""), std::string::npos);
  EXPECT_EQ(create_text.find("\"requestedSessionTimeout\""), std::string::npos);

  const auto activate_json =
      EncodeJson(RequestMessage{.request_handle = 12,
                                .body = ActivateSessionRequest{
                                    .authentication_token = NumericNode(21, 3),
                                    .user_name = opcua::LocalizedText{u"op"},
                                    .password = opcua::LocalizedText{u"pw"},
                                }});
  const auto activate_text = boost::json::serialize(activate_json);
  // The session binding is the token in the RequestHeader; the credentials are
  // carried in the UserIdentityToken ExtensionObject.
  EXPECT_NE(activate_text.find("\"RequestHeader\""), std::string::npos);
  EXPECT_NE(activate_text.find("\"AuthenticationToken\""), std::string::npos);
  EXPECT_NE(activate_text.find("\"UserIdentityToken\""), std::string::npos);
  EXPECT_EQ(activate_text.find("\"authenticationToken\""), std::string::npos);
}

TEST(JsonCodecTest, RoundTripsHistoryReadRawRequest) {
  // The conformant ReadProcessedDetails ties the aggregate start to the read
  // range start, so aggregation.start_time round-trips as `from`.
  const auto from = ParseTime("2026-04-19 10:00:00");
  const HistoryReadRawDetails details{
      .node_id = NumericNode(1),
      .from = from,
      .to = ParseTime("2026-04-19 12:00:00"),
      .max_count = 123,
      .aggregation = {.start_time = from,
                      .interval = opcua::Duration::FromMinutes(15),
                      .aggregate_type = NumericNode(44)},
      .release_continuation_point = true,
      .continuation_point = {'a', 'b', 'c'}};

  auto json =
      EncodeJson(ServiceRequest{history_conversion::ToWireRawRequest(details)});
  auto decoded = *DecodeServiceRequest(json);
  // OPC UA has a single HistoryRead service; the HistoryReadDetails extension
  // object selects raw vs events (Part 4 §5.10.3). The old HistoryReadRaw /
  // HistoryReadEvents split is gone.
  const auto serialized = boost::json::serialize(json);
  EXPECT_NE(serialized.find("HistoryRead"), std::string::npos);
  EXPECT_EQ(serialized.find("HistoryReadRaw"), std::string::npos);
  EXPECT_EQ(serialized.find("HistoryReadEvents"), std::string::npos);
  // The details must ride as a JSON body, not a base64 binary one: a JSON-only
  // peer (the web client) has no binary decoder.
  const auto& encoded_details =
      json.as_object().at("body").as_object().at("HistoryReadDetails");
  EXPECT_FALSE(encoded_details.as_object().contains("UaEncoding"));
  EXPECT_TRUE(encoded_details.as_object().at("UaBody").is_object());
  const auto* typed = std::get_if<ua::HistoryReadRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  const auto managed = history_conversion::ToManaged(*typed);
  ASSERT_TRUE(managed.has_value());
  const auto* raw = std::get_if<HistoryReadRawDetails>(&managed->details);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(raw->node_id, details.node_id);
  EXPECT_EQ(raw->from, details.from);
  EXPECT_EQ(raw->to, details.to);
  EXPECT_EQ(raw->aggregation, details.aggregation);
  EXPECT_EQ(raw->release_continuation_point,
            details.release_continuation_point);
  EXPECT_EQ(raw->continuation_point, details.continuation_point);
}

// --- Trace context over the websocket transport -----------------------------

// The websocket envelope is opcuapp's own `{requestHandle, service, body}`
// shape, but `body` is the generated wire request and so embeds the spec's
// RequestHeader. That is where trace context lives, and reading it is what
// joins a browser-client request to the trace it belongs to instead of starting
// a fresh root at the proxy.
TEST(JsonCodecTest, RequestMessageCarriesTheTraceContextFromItsRequestHeader) {
  constexpr std::string_view kTraceParent =
      "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

  ua::ReadRequest wire;
  wire.request_header = ua::MakeRequestHeader(NodeId{}, 3, kTraceParent);
  const boost::json::value json{{"requestHandle", 3},
                                {"service", "Read"},
                                {"body", ua::EncodeJson(wire)}};

  const auto decoded = DecodeRequestMessage(json);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->trace_parent, kTraceParent);
}

// A request without trace context decodes exactly as before — the field is
// optional and its absence must never be an error.
TEST(JsonCodecTest, RequestMessageWithoutTraceContextDecodesUntraced) {
  ua::ReadRequest wire;
  const boost::json::value json{{"requestHandle", 4},
                                {"service", "Read"},
                                {"body", ua::EncodeJson(wire)}};

  const auto decoded = DecodeRequestMessage(json);
  ASSERT_TRUE(decoded.ok());
  EXPECT_TRUE(decoded->trace_parent.empty());
}

TEST(JsonCodecTest, RoundTripsHistoryReadRawRequestMessage) {
  RequestMessage request{.request_handle = 8,
                         .body = history_conversion::ToWireRawRequest(
                             {.node_id = NumericNode(101, 1)})};

  const auto encoded = boost::json::serialize(EncodeJson(request));
  const auto decoded = *DecodeRequestMessage(boost::json::parse(encoded));

  EXPECT_EQ(decoded.request_handle, request.request_handle);
  const auto* typed = std::get_if<ua::HistoryReadRequest>(&decoded.body);
  ASSERT_NE(typed, nullptr);
  const auto managed = history_conversion::ToManaged(*typed);
  ASSERT_TRUE(managed.has_value());
  const auto* raw = std::get_if<HistoryReadRawDetails>(&managed->details);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(raw->node_id, NumericNode(101, 1));
}

TEST(JsonCodecTest, RoundTripsHistoryReadEventsRequest) {
  const HistoryReadEventsDetails details{
      .node_id = NumericNode(2),
      .from = ParseTime("2026-04-19 08:00:00"),
      .to = ParseTime("2026-04-19 09:00:00"),
      .filter = {.types = opcua::EventFilter::UNACKED,
                 .of_type = {NumericNode(77), NumericNode(78)},
                 .child_of = {NumericNode(79)}}};

  auto decoded = *DecodeServiceRequest(EncodeJson(
      ServiceRequest{history_conversion::ToWireEventsRequest(details)}));
  const auto* typed = std::get_if<ua::HistoryReadRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  const auto managed = history_conversion::ToManaged(*typed);
  ASSERT_TRUE(managed.has_value());
  const auto* events = std::get_if<HistoryReadEventsDetails>(&managed->details);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(events->node_id, details.node_id);
  EXPECT_EQ(events->from, details.from);
  EXPECT_EQ(events->to, details.to);
  EXPECT_EQ(events->filter, details.filter);
}

TEST(JsonCodecTest, RoundTripsCallRequestWithScalarAndArrayVariants) {
  ua::CallRequest request{
      .methods_to_call = {
          {.object_id = NumericNode(10),
           .method_id = NumericNode(11),
           .input_arguments = {
               opcua::Variant{true},
               opcua::Variant{std::vector<opcua::Int32>{1, 2, 3}},
               opcua::Variant{opcua::LocalizedText{u"hello"}},
               opcua::Variant{NumericNode(12)}}}}};

  auto decoded = *DecodeServiceRequest(EncodeJson(ServiceRequest{request}));
  const auto* typed = std::get_if<ua::CallRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->methods_to_call.size(), 1u);
  EXPECT_EQ(typed->methods_to_call[0].object_id,
            request.methods_to_call[0].object_id);
  EXPECT_EQ(typed->methods_to_call[0].method_id,
            request.methods_to_call[0].method_id);
  EXPECT_EQ(typed->methods_to_call[0].input_arguments,
            request.methods_to_call[0].input_arguments);
}

TEST(JsonCodecTest, CallWireShapeUsesSpecFieldNames) {
  const auto json = EncodeJson(ServiceRequest{ua::CallRequest{
      .methods_to_call = {
          {.object_id = NumericNode(10),
           .method_id = NumericNode(11),
           .input_arguments = {opcua::Variant{opcua::Int32{5}}}}}}});

  const auto& body = json.as_object().at("body").as_object();
  EXPECT_TRUE(body.contains("MethodsToCall"));
  EXPECT_FALSE(body.contains("Methods"));

  const auto& method = body.at("MethodsToCall").as_array().at(0).as_object();
  EXPECT_TRUE(method.contains("InputArguments"));
  EXPECT_FALSE(method.contains("Arguments"));
}

TEST(JsonCodecTest, RoundTripsOpaqueExtensionObjectVariants) {
  const boost::json::value scalar_payload = boost::json::parse(
      R"({"Kind":"AlarmFilter","Severity":500,"Fields":["Message","SourceName"]})");
  const boost::json::value array_payload_1 =
      boost::json::parse(R"({"Kind":"Operand","NodeId":"ns=2;i=5001"})");
  const boost::json::value array_payload_2 = boost::json::parse(
      R"({"Kind":"Operand","BrowsePath":["Objects","Motor1"]})");

  ua::CallRequest request{
      .methods_to_call = {
          {.object_id = NumericNode(120),
           .method_id = NumericNode(121),
           .input_arguments = {
               opcua::Variant{opcua::ExtensionObject{
                   opcua::ExpandedNodeId{NumericNode(122), "urn:test", 3},
                   scalar_payload}},
               opcua::Variant{std::vector<opcua::ExtensionObject>{
                   {opcua::ExpandedNodeId{NumericNode(123)}, array_payload_1},
                   {opcua::ExpandedNodeId{NumericNode(124), "urn:test", 4},
                    array_payload_2}}}}}}};

  const auto decoded =
      *DecodeServiceRequest(EncodeJson(ServiceRequest{request}));
  const auto* typed = std::get_if<ua::CallRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->methods_to_call.size(), 1u);
  ASSERT_EQ(typed->methods_to_call[0].input_arguments.size(), 2u);

  const auto& scalar_extension = typed->methods_to_call[0]
                                     .input_arguments[0]
                                     .get<opcua::ExtensionObject>();
  EXPECT_EQ(scalar_extension.data_type_id(),
            (opcua::ExpandedNodeId{NumericNode(122), "urn:test", 3}));
  const auto* scalar_decoded =
      std::any_cast<boost::json::value>(&scalar_extension.value());
  ASSERT_NE(scalar_decoded, nullptr);
  EXPECT_EQ(*scalar_decoded, scalar_payload);

  const auto& array_extensions =
      typed->methods_to_call[0]
          .input_arguments[1]
          .get<std::vector<opcua::ExtensionObject>>();
  ASSERT_EQ(array_extensions.size(), 2u);
  EXPECT_EQ(array_extensions[0].data_type_id(),
            opcua::ExpandedNodeId{NumericNode(123)});
  EXPECT_EQ(array_extensions[1].data_type_id(),
            (opcua::ExpandedNodeId{NumericNode(124), "urn:test", 4}));
  const auto* array_decoded_1 =
      std::any_cast<boost::json::value>(&array_extensions[0].value());
  const auto* array_decoded_2 =
      std::any_cast<boost::json::value>(&array_extensions[1].value());
  ASSERT_NE(array_decoded_1, nullptr);
  ASSERT_NE(array_decoded_2, nullptr);
  EXPECT_EQ(*array_decoded_1, array_payload_1);
  EXPECT_EQ(*array_decoded_2, array_payload_2);
}

TEST(JsonCodecTest, RoundTripsNodeManagementRequests) {
  ua::VariableAttributes variable_attributes;
  variable_attributes.display_name = opcua::LocalizedText{u"Pressure"};
  variable_attributes.data_type = NumericNode(103);
  variable_attributes.value = opcua::Variant{42.5};
  ua::AddNodesRequest add_nodes{
      .nodes_to_add = {
          {.parent_node_id = opcua::ExpandedNodeId{NumericNode(101)},
           .requested_new_node_id = opcua::ExpandedNodeId{NumericNode(100)},
           .browse_name = {"Pressure", 3},
           .node_class = ua::NodeClass::Variable,
           .node_attributes = ua::ToExtensionObject(variable_attributes),
           .type_definition = opcua::ExpandedNodeId{NumericNode(102)}}}};
  ua::DeleteNodesRequest delete_nodes{
      .nodes_to_delete = {
          {.node_id = NumericNode(104), .delete_target_references = true}}};
  ua::AddReferencesRequest add_refs{
      .references_to_add = {
          {.source_node_id = NumericNode(105),
           .reference_type_id = NumericNode(106),
           .is_forward = false,
           .target_server_uri = "opc.tcp://server",
           .target_node_id =
               opcua::ExpandedNodeId{NumericNode(107), "urn:test", 2},
           .target_node_class = opcua::ua::NodeClass::Object}}};
  ua::DeleteReferencesRequest delete_refs{
      .references_to_delete = {
          {.source_node_id = NumericNode(108),
           .reference_type_id = NumericNode(109),
           .is_forward = true,
           .target_node_id = opcua::ExpandedNodeId{NumericNode(110)},
           .delete_bidirectional = false}}};

  const auto decoded_add_nodes = std::get<ua::AddNodesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{add_nodes})));
  ASSERT_EQ(decoded_add_nodes.nodes_to_add.size(), 1u);
  const auto& add_node_item = decoded_add_nodes.nodes_to_add[0];
  EXPECT_EQ(add_node_item.parent_node_id,
            add_nodes.nodes_to_add[0].parent_node_id);
  EXPECT_EQ(add_node_item.requested_new_node_id,
            add_nodes.nodes_to_add[0].requested_new_node_id);
  EXPECT_EQ(add_node_item.browse_name, add_nodes.nodes_to_add[0].browse_name);
  EXPECT_EQ(add_node_item.node_class, ua::NodeClass::Variable);
  EXPECT_EQ(add_node_item.type_definition,
            add_nodes.nodes_to_add[0].type_definition);
  ua::VariableAttributes decoded_attributes;
  ASSERT_TRUE(ua::FromExtensionObject(add_node_item.node_attributes,
                                      decoded_attributes));
  EXPECT_EQ(decoded_attributes.display_name, opcua::LocalizedText{u"Pressure"});
  EXPECT_EQ(decoded_attributes.data_type, NumericNode(103));
  EXPECT_EQ(decoded_attributes.value, opcua::Variant{42.5});

  const auto decoded_delete_nodes = std::get<ua::DeleteNodesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{delete_nodes})));
  ASSERT_EQ(decoded_delete_nodes.nodes_to_delete.size(), 1u);
  EXPECT_EQ(decoded_delete_nodes.nodes_to_delete[0].node_id,
            delete_nodes.nodes_to_delete[0].node_id);
  EXPECT_EQ(decoded_delete_nodes.nodes_to_delete[0].delete_target_references,
            delete_nodes.nodes_to_delete[0].delete_target_references);

  const auto decoded_add_refs = std::get<ua::AddReferencesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{add_refs})));
  ASSERT_EQ(decoded_add_refs.references_to_add.size(), 1u);
  EXPECT_EQ(decoded_add_refs.references_to_add[0].source_node_id,
            add_refs.references_to_add[0].source_node_id);
  EXPECT_EQ(decoded_add_refs.references_to_add[0].reference_type_id,
            add_refs.references_to_add[0].reference_type_id);
  EXPECT_EQ(decoded_add_refs.references_to_add[0].is_forward,
            add_refs.references_to_add[0].is_forward);
  EXPECT_EQ(decoded_add_refs.references_to_add[0].target_server_uri,
            add_refs.references_to_add[0].target_server_uri);
  EXPECT_EQ(decoded_add_refs.references_to_add[0].target_node_id,
            add_refs.references_to_add[0].target_node_id);
  EXPECT_EQ(decoded_add_refs.references_to_add[0].target_node_class,
            add_refs.references_to_add[0].target_node_class);

  const auto decoded_delete_refs = std::get<ua::DeleteReferencesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{delete_refs})));
  ASSERT_EQ(decoded_delete_refs.references_to_delete.size(), 1u);
  EXPECT_EQ(decoded_delete_refs.references_to_delete[0].source_node_id,
            delete_refs.references_to_delete[0].source_node_id);
  EXPECT_EQ(decoded_delete_refs.references_to_delete[0].reference_type_id,
            delete_refs.references_to_delete[0].reference_type_id);
  EXPECT_EQ(decoded_delete_refs.references_to_delete[0].is_forward,
            delete_refs.references_to_delete[0].is_forward);
  EXPECT_EQ(decoded_delete_refs.references_to_delete[0].target_node_id,
            delete_refs.references_to_delete[0].target_node_id);
  EXPECT_EQ(decoded_delete_refs.references_to_delete[0].delete_bidirectional,
            delete_refs.references_to_delete[0].delete_bidirectional);
}

TEST(JsonCodecTest, NodeManagementWireShapeUsesSpecFieldNames) {
  const auto add_nodes = EncodeJson(ServiceRequest{ua::AddNodesRequest{
      .nodes_to_add = {
          {.parent_node_id = opcua::ExpandedNodeId{NumericNode(101)},
           .requested_new_node_id = opcua::ExpandedNodeId{NumericNode(100)},
           .node_class = ua::NodeClass::Variable,
           .node_attributes = ua::ToExtensionObject(ua::VariableAttributes{}),
           .type_definition = opcua::ExpandedNodeId{NumericNode(102)}}}}});
  const auto delete_nodes = EncodeJson(ServiceRequest{ua::DeleteNodesRequest{
      .nodes_to_delete = {
          {.node_id = NumericNode(104), .delete_target_references = true}}}});
  const auto add_refs = EncodeJson(ServiceRequest{ua::AddReferencesRequest{
      .references_to_add = {
          {.source_node_id = NumericNode(105),
           .reference_type_id = NumericNode(106),
           // Non-default so the conformant compact codec emits IsForward.
           .is_forward = true,
           .target_server_uri = "opc.tcp://server",
           .target_node_id =
               opcua::ExpandedNodeId{NumericNode(107), "urn:test", 2},
           .target_node_class = opcua::ua::NodeClass::Object}}}});
  const auto delete_refs =
      EncodeJson(ServiceRequest{ua::DeleteReferencesRequest{
          .references_to_delete = {
              {.source_node_id = NumericNode(108),
               .reference_type_id = NumericNode(109),
               .is_forward = true,
               .target_node_id = opcua::ExpandedNodeId{NumericNode(110)},
               .delete_bidirectional = false}}}});

  const auto& add_nodes_body = add_nodes.as_object().at("body").as_object();
  EXPECT_TRUE(add_nodes_body.contains("NodesToAdd"));
  EXPECT_FALSE(add_nodes_body.contains("Items"));

  const auto& delete_nodes_body =
      delete_nodes.as_object().at("body").as_object();
  EXPECT_TRUE(delete_nodes_body.contains("NodesToDelete"));
  EXPECT_FALSE(delete_nodes_body.contains("Items"));

  const auto& add_refs_body = add_refs.as_object().at("body").as_object();
  EXPECT_TRUE(add_refs_body.contains("ReferencesToAdd"));
  EXPECT_FALSE(add_refs_body.contains("Items"));
  EXPECT_TRUE(add_refs_body.at("ReferencesToAdd")
                  .as_array()
                  .at(0)
                  .as_object()
                  .contains("IsForward"));

  const auto& delete_refs_body = delete_refs.as_object().at("body").as_object();
  EXPECT_TRUE(delete_refs_body.contains("ReferencesToDelete"));
  EXPECT_FALSE(delete_refs_body.contains("Items"));
  EXPECT_TRUE(delete_refs_body.at("ReferencesToDelete")
                  .as_array()
                  .at(0)
                  .as_object()
                  .contains("IsForward"));
}

TEST(JsonCodecTest, RoundTripsHistoryReadResponses) {
  const HistoryReadRawResult raw_result{
      .values = {opcua::DataValue{
          opcua::Variant{12.5}, opcua::Qualifier{opcua::Qualifier::MANUAL},
          ParseTime("2026-04-19 12:00:00"), ParseTime("2026-04-19 12:00:01")}},
      .continuation_point = {'x', 'y'}};

  opcua::Event event;
  event.event_type_id = NumericNode(200);
  event.event_id = 201;
  event.time = ParseTime("2026-04-19 11:00:00");
  event.receive_time = ParseTime("2026-04-19 11:00:01");
  event.change_mask = opcua::Event::EVT_VAL;
  event.severity = opcua::kSeverityWarning;
  event.source_node_id = NumericNode(202);
  event.user_id = NumericNode(203);
  event.value = opcua::Variant{std::string{"trip"}};
  event.qualifier = opcua::Qualifier{opcua::Qualifier::OFFLINE};
  // Locale-qualified message: the JSON LocalizedText object must carry the
  // `Locale` field through the round trip (OPC UA Part 6 §5.4.2.14).
  event.message = opcua::LocalizedText{"ru", u"Alarm"};
  event.acked = true;
  event.acknowledged_time = ParseTime("2026-04-19 11:05:00");
  event.acknowledged_user_id = NumericNode(204);

  const HistoryReadEventsResult events_result{.events = {event}};

  // DataValue.qualifier is not part of the spec wire form (§5.4.2.17), so
  // round-trip per-field rather than via vector equality (which would
  // print-loop on Qualifier diffs through GTest's Variant printer).
  const auto decoded_raw =
      history_conversion::ToManagedRawResult(std::get<ua::HistoryReadResponse>(
          *DecodeServiceResponse(EncodeJson(ServiceResponse{
              history_conversion::ToWireRawResponse(raw_result)}))));
  ASSERT_TRUE(decoded_raw.ok()) << decoded_raw.status();
  EXPECT_EQ(decoded_raw->continuation_point, raw_result.continuation_point);
  ASSERT_EQ(decoded_raw->values.size(), raw_result.values.size());
  EXPECT_TRUE(decoded_raw->values[0].value == raw_result.values[0].value);
  EXPECT_EQ(decoded_raw->values[0].status_code,
            raw_result.values[0].status_code);
  EXPECT_EQ(decoded_raw->values[0].source_timestamp,
            raw_result.values[0].source_timestamp);
  EXPECT_EQ(decoded_raw->values[0].server_timestamp,
            raw_result.values[0].server_timestamp);

  const auto decoded_events = history_conversion::ToManagedEventsResult(
      std::get<ua::HistoryReadResponse>(*DecodeServiceResponse(
          EncodeJson(ServiceResponse{history_conversion::ToWireEventsResponse(
              events_result, DefaultEventFieldPaths())}))));
  ASSERT_TRUE(decoded_events.ok()) << decoded_events.status();
  ASSERT_EQ(decoded_events->events.size(), 1u);
  // Events now round-trip through the conformant projection onto the default
  // select clauses (ua::HistoryEvent), so source_name is re-derived from the
  // SourceNode field on reconstruction rather than carried verbatim; normalize
  // it before comparing the selected fields.
  auto normalized = decoded_events->events;
  normalized[0].source_name = events_result.events[0].source_name;
  EXPECT_EQ(normalized, events_result.events);
}

// The response's HistoryData must reach the wire as a JSON body. history_
// conversion builds binary-bodied ExtensionObjects because it is transport-
// agnostic, so the JSON transport transcodes on the way out; without that a
// JSON-only peer receives a base64 ByteString it cannot decode.
TEST(JsonCodecTest, HistoryReadResponseCarriesAJsonBodiedPayload) {
  const HistoryReadRawResult raw_result{
      .values = {opcua::DataValue{opcua::Variant{12.5}, opcua::Qualifier{},
                                  ParseTime("2026-04-19 12:00:00"),
                                  ParseTime("2026-04-19 12:00:01")}}};

  const auto json = EncodeJson(
      ServiceResponse{history_conversion::ToWireRawResponse(raw_result)});
  const auto serialized = boost::json::serialize(json);
  EXPECT_NE(serialized.find("HistoryRead"), std::string::npos);
  EXPECT_EQ(serialized.find("HistoryReadRaw"), std::string::npos);

  const auto& results =
      json.as_object().at("body").as_object().at("Results").as_array();
  ASSERT_EQ(results.size(), 1u);
  const auto& history_data =
      results[0].as_object().at("HistoryData").as_object();
  EXPECT_FALSE(history_data.contains("UaEncoding"));
  ASSERT_TRUE(history_data.at("UaBody").is_object());
  EXPECT_TRUE(history_data.at("UaBody").as_object().contains("DataValues"));
}

// HistoryUpdate travels as its conformant service too (Part 4 §5.10.5), with
// the update detail as a JSON-bodied ExtensionObject. It previously had a
// bespoke `HistoryUpdate` body shape and no coverage in this file at all.
TEST(JsonCodecTest, RoundTripsHistoryUpdateDataRequest) {
  UpdateDataDetails details;
  details.node_id = NumericNode(7);
  details.perform_insert_replace = PerformUpdateType::Insert;
  details.values = {opcua::DataValue{opcua::Variant{42.5}, opcua::Qualifier{},
                                     ParseTime("2026-04-19 09:00:00"),
                                     ParseTime("2026-04-19 09:00:01")}};

  const auto json = EncodeJson(ServiceRequest{history_conversion::ToWire(
      history_conversion::HistoryUpdateDetails{details})});
  const auto serialized = boost::json::serialize(json);
  EXPECT_NE(serialized.find("HistoryUpdate"), std::string::npos);

  // The detail must be a JSON body, not a base64 binary one.
  const auto& encoded_details = json.as_object()
                                    .at("body")
                                    .as_object()
                                    .at("HistoryUpdateDetails")
                                    .as_array();
  ASSERT_EQ(encoded_details.size(), 1u);
  EXPECT_FALSE(encoded_details[0].as_object().contains("UaEncoding"));
  EXPECT_TRUE(encoded_details[0].as_object().at("UaBody").is_object());

  const auto decoded = *DecodeServiceRequest(json);
  const auto* typed = std::get_if<ua::HistoryUpdateRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  const auto managed = history_conversion::ToManaged(*typed);
  ASSERT_TRUE(managed.has_value());
  const auto* data = std::get_if<UpdateDataDetails>(&*managed);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->node_id, details.node_id);
  EXPECT_EQ(data->perform_insert_replace, details.perform_insert_replace);
  ASSERT_EQ(data->values.size(), 1u);
  EXPECT_TRUE(data->values[0].value == details.values[0].value);
  EXPECT_EQ(data->values[0].source_timestamp,
            details.values[0].source_timestamp);
}

// The response carries per-operation status codes and no ExtensionObject, so it
// needs no body transcoding — but it must still travel under the conformant
// service name and shape.
TEST(JsonCodecTest, RoundTripsHistoryUpdateResponse) {
  const std::vector<StatusCode> operation_results{
      StatusCode{0}, StatusCode{opcua::StatusCode::Bad_OutOfRange}};

  const auto decoded = history_conversion::ToManaged(
      std::get<ua::HistoryUpdateResponse>(*DecodeServiceResponse(EncodeJson(
          ServiceResponse{history_conversion::ToWire(operation_results)}))));
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, operation_results);
}

// A HistoryReadRaw failure round-trips as the per-node status alone -- the
// StatusOr carries no values with a bad status. OPC UA Part 4 §5.11.3
// HistoryRead, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.3
TEST(JsonCodecTest, RoundTripsFailedHistoryReadRawResponse) {
  const opcua::Status status = opcua::Status::FromFullCode(0x80030002u);
  const auto decoded = history_conversion::ToManagedRawResult(
      std::get<ua::HistoryReadResponse>(*DecodeServiceResponse(EncodeJson(
          ServiceResponse{history_conversion::ToWireRawResponse(status)}))));
  EXPECT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.status().full_code(), status.full_code());
}

TEST(JsonCodecTest, RoundTripsPhase0Responses) {
  ua::ReadResponse read{
      .results = {opcua::DataValue{
          opcua::Variant{opcua::LocalizedText{u"Pump"}},
          opcua::Qualifier{opcua::Qualifier::MANUAL},
          ParseTime("2026-04-19 10:10:00"), ParseTime("2026-04-19 10:10:01")}}};
  ua::WriteResponse write;
  write.response_header.service_result = opcua::StatusCode::Bad_NoCommunication;
  write.results = {Status{opcua::StatusCode::Bad_NoCommunication}};
  ua::BrowseResponse browse{
      .results = {{.status_code = Status{opcua::StatusCode::Good},
                   .continuation_point = {'c', 'p'},
                   .references = {
                       {.reference_type_id = NumericNode(301),
                        .is_forward = false,
                        .node_id = opcua::ExpandedNodeId{NumericNode(302)}}}}}};
  ua::BrowseNextResponse browse_next{
      .results = {{.status_code =
                       Status{opcua::StatusCode::Bad_ContinuationPointInvalid}},
                  {.status_code = Status{opcua::StatusCode::Good},
                   .references = {
                       {.reference_type_id = NumericNode(304),
                        .is_forward = true,
                        .node_id = opcua::ExpandedNodeId{NumericNode(305)}}}}}};
  ua::TranslateBrowsePathsToNodeIdsResponse translate;
  translate.results = {
      {.status_code = opcua::Status{opcua::StatusCode::Good},
       .targets = {
           {.target_id = opcua::ExpandedNodeId{NumericNode(303), "urn:test", 2},
            .remaining_path_index = 1}}}};

  // Per OPC UA Part 6 §5.4.2.17, DataValue carries Value, Status,
  // Source/ServerTimestamp, and Source/ServerPicoseconds — no Qualifier.
  // The opcua::Qualifier is deliberately dropped on the wire, so the
  // decoded DataValue's qualifier is the default (assert per-field).
  const auto decoded_read = std::get<ua::ReadResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{read})));
  ASSERT_EQ(decoded_read.results.size(), 1u);
  EXPECT_TRUE(decoded_read.results[0].value == read.results[0].value);
  EXPECT_EQ(decoded_read.results[0].source_timestamp,
            read.results[0].source_timestamp);
  EXPECT_EQ(decoded_read.results[0].server_timestamp,
            read.results[0].server_timestamp);
  EXPECT_EQ(decoded_read.results[0].status_code, read.results[0].status_code);

  const auto decoded_write = std::get<ua::WriteResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{write})));
  EXPECT_EQ(decoded_write.response_header.service_result,
            write.response_header.service_result);
  ASSERT_EQ(decoded_write.results.size(), 1u);
  EXPECT_EQ(decoded_write.results[0].code(),
            opcua::StatusCode::Bad_NoCommunication);

  const auto decoded_browse = std::get<ua::BrowseResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{browse})));
  ASSERT_EQ(decoded_browse.results.size(), 1u);
  EXPECT_EQ(decoded_browse.results[0].status_code.code(),
            browse.results[0].status_code.code());
  EXPECT_EQ(decoded_browse.results[0].continuation_point,
            browse.results[0].continuation_point);
  ASSERT_EQ(decoded_browse.results[0].references.size(), 1u);
  EXPECT_EQ(decoded_browse.results[0].references[0].node_id.node_id(),
            NumericNode(302));
  EXPECT_FALSE(decoded_browse.results[0].references[0].is_forward);

  const auto decoded_browse_next = std::get<ua::BrowseNextResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{browse_next})));
  ASSERT_EQ(decoded_browse_next.results.size(), 2u);
  EXPECT_EQ(decoded_browse_next.results[0].status_code.code(),
            opcua::StatusCode::Bad_ContinuationPointInvalid);
  ASSERT_EQ(decoded_browse_next.results[1].references.size(), 1u);
  EXPECT_EQ(decoded_browse_next.results[1].references[0].node_id.node_id(),
            NumericNode(305));

  const auto decoded_translate =
      std::get<ua::TranslateBrowsePathsToNodeIdsResponse>(
          *DecodeServiceResponse(EncodeJson(ServiceResponse{translate})));
  ASSERT_EQ(decoded_translate.results.size(), 1u);
  EXPECT_EQ(decoded_translate.results[0].status_code.code(),
            translate.results[0].status_code.code());
  ASSERT_EQ(decoded_translate.results[0].targets.size(), 1u);
  EXPECT_EQ(decoded_translate.results[0].targets[0].target_id,
            translate.results[0].targets[0].target_id);
  EXPECT_EQ(decoded_translate.results[0].targets[0].remaining_path_index,
            translate.results[0].targets[0].remaining_path_index);
}

TEST(JsonCodecTest, CallResponseWireShapeUsesSpecFields) {
  // Use a non-Good result status: the conformant compact JSON encoding
  // (OPC UA Part 6 §5.4.2) omits a StatusCode field whose value is Good(0), so
  // asserting the field's presence requires a non-zero status.
  ua::CallResponse response{
      .results = {{.status_code = opcua::StatusCode::Bad_MethodInvalid,
                   .input_argument_results = {opcua::Status{
                       opcua::StatusCode::Bad_TypeDefinitionInvalid}},
                   .output_arguments = {opcua::Variant{opcua::Int32{9}}}}}};

  const auto encoded = EncodeJson(ServiceResponse{response});
  const auto& result = encoded.as_object()
                           .at("body")
                           .as_object()
                           .at("Results")
                           .as_array()
                           .at(0)
                           .as_object();
  EXPECT_TRUE(result.contains("StatusCode"));
  EXPECT_FALSE(result.contains("Status"));
  EXPECT_TRUE(result.contains("InputArgumentResults"));
  EXPECT_TRUE(result.contains("OutputArguments"));

  const auto decoded =
      std::get<ua::CallResponse>(*DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  EXPECT_EQ(decoded.results[0].status_code.code(),
            opcua::StatusCode::Bad_MethodInvalid);
  ASSERT_EQ(decoded.results[0].input_argument_results.size(), 1u);
  EXPECT_EQ(decoded.results[0].input_argument_results[0].code(),
            opcua::StatusCode::Bad_TypeDefinitionInvalid);
  EXPECT_EQ(decoded.results[0].output_arguments,
            (std::vector<opcua::Variant>{opcua::Variant{opcua::Int32{9}}}));
}

TEST(JsonCodecTest, RoundTripsSessionResponseMessagesAndFault) {
  ResponseMessage create{.request_handle = 21,
                         .body = CreateSessionResponse{
                             .status = opcua::StatusCode::Good,
                             .session_id = NumericNode(30),
                             .authentication_token = NumericNode(31, 3),
                             .server_nonce = {1, 2, 3, 4},
                             .revised_timeout = opcua::Duration::FromMinutes(5),
                         }};
  ResponseMessage activate{
      .request_handle = 22,
      .body = ActivateSessionResponse{
          .status = opcua::StatusCode::Bad_IdentityTokenRejected,
      }};
  ResponseMessage close{
      .request_handle = 23,
      .body = CloseSessionResponse{.status = opcua::StatusCode::Good}};
  ResponseMessage fault{
      .request_handle = 24,
      .body = ServiceFault{.status = opcua::StatusCode::Bad_NoCommunication}};

  const auto decoded_create = *DecodeResponseMessage(EncodeJson(create));
  EXPECT_EQ(decoded_create.request_handle, create.request_handle);
  const auto* create_body =
      std::get_if<CreateSessionResponse>(&decoded_create.body);
  ASSERT_NE(create_body, nullptr);
  EXPECT_EQ(create_body->status, opcua::StatusCode::Good);
  EXPECT_EQ(create_body->session_id, NumericNode(30));
  EXPECT_EQ(create_body->authentication_token, NumericNode(31, 3));
  EXPECT_EQ(create_body->server_nonce, (opcua::ByteString{1, 2, 3, 4}));
  EXPECT_EQ(create_body->revised_timeout, opcua::Duration::FromMinutes(5));

  const auto decoded_activate = *DecodeResponseMessage(EncodeJson(activate));
  EXPECT_EQ(decoded_activate.request_handle, activate.request_handle);
  const auto* activate_body =
      std::get_if<ActivateSessionResponse>(&decoded_activate.body);
  ASSERT_NE(activate_body, nullptr);
  EXPECT_EQ(activate_body->status.code(),
            opcua::StatusCode::Bad_IdentityTokenRejected);

  const auto decoded_close = *DecodeResponseMessage(EncodeJson(close));
  EXPECT_EQ(decoded_close.request_handle, close.request_handle);
  const auto* close_body =
      std::get_if<CloseSessionResponse>(&decoded_close.body);
  ASSERT_NE(close_body, nullptr);
  EXPECT_EQ(close_body->status.code(), opcua::StatusCode::Good);

  const auto decoded_fault = *DecodeResponseMessage(EncodeJson(fault));
  EXPECT_EQ(decoded_fault.request_handle, fault.request_handle);
  const auto* fault_body = std::get_if<ServiceFault>(&decoded_fault.body);
  ASSERT_NE(fault_body, nullptr);
  EXPECT_EQ(fault_body->status.code(), opcua::StatusCode::Bad_NoCommunication);
}

TEST(JsonCodecTest, RoundTripsDiscoveryMessages) {
  RequestMessage find_request{
      .request_handle = 40,
      .body = FindServersRequest{.endpoint_url = "opc.tcp://host:4840",
                                 .locale_ids = {"en"},
                                 .server_uris = {"urn:server"}}};
  RequestMessage endpoints_request{
      .request_handle = 41,
      .body = GetEndpointsRequest{.endpoint_url = "opc.tcp://host:4840",
                                  .profile_uris = {"http://profile"}}};
  ResponseMessage find_response{
      .request_handle = 40,
      .body = FindServersResponse{
          .servers = {{.application_uri = "urn:server",
                       .application_name = opcua::LocalizedText{u"Server"},
                       .application_type = opcua::ApplicationType::Server,
                       .discovery_urls = {"opc.tcp://host:4840"}}}}};
  ResponseMessage endpoints_response{
      .request_handle = 41,
      .body = GetEndpointsResponse{
          .endpoints = {
              {.endpoint_url = "opc.tcp://host:4840",
               .security_mode = opcua::MessageSecurityMode::None,
               .security_policy_uri = "http://none",
               .user_identity_tokens = {{.policy_id = "anon",
                                         .token_type =
                                             opcua::UserTokenType::Anonymous}},
               .security_level = 1}}}};

  const auto decoded_find = *DecodeRequestMessage(EncodeJson(find_request));
  const auto* find_body = std::get_if<FindServersRequest>(&decoded_find.body);
  ASSERT_NE(find_body, nullptr);
  EXPECT_EQ(find_body->endpoint_url, "opc.tcp://host:4840");
  EXPECT_EQ(find_body->server_uris, (std::vector<std::string>{"urn:server"}));

  const auto decoded_endpoints_req =
      *DecodeRequestMessage(EncodeJson(endpoints_request));
  ASSERT_TRUE(
      std::holds_alternative<GetEndpointsRequest>(decoded_endpoints_req.body));

  const auto decoded_find_resp =
      *DecodeResponseMessage(EncodeJson(find_response));
  const auto* find_resp_body =
      std::get_if<FindServersResponse>(&decoded_find_resp.body);
  ASSERT_NE(find_resp_body, nullptr);
  ASSERT_EQ(find_resp_body->servers.size(), 1u);
  EXPECT_EQ(find_resp_body->servers[0].application_uri, "urn:server");
  EXPECT_EQ(find_resp_body->servers[0].application_type,
            opcua::ApplicationType::Server);

  const auto decoded_endpoints =
      *DecodeResponseMessage(EncodeJson(endpoints_response));
  const auto* endpoints_body =
      std::get_if<GetEndpointsResponse>(&decoded_endpoints.body);
  ASSERT_NE(endpoints_body, nullptr);
  ASSERT_EQ(endpoints_body->endpoints.size(), 1u);
  const auto& endpoint = endpoints_body->endpoints[0];
  EXPECT_EQ(endpoint.endpoint_url, "opc.tcp://host:4840");
  EXPECT_EQ(endpoint.security_mode, opcua::MessageSecurityMode::None);
  ASSERT_EQ(endpoint.user_identity_tokens.size(), 1u);
  EXPECT_EQ(endpoint.user_identity_tokens[0].token_type,
            opcua::UserTokenType::Anonymous);
  EXPECT_EQ(endpoint.security_level, 1);
}

TEST(JsonCodecTest, RoundTripsServiceMessagesWithEnvelope) {
  RequestMessage request{
      .request_handle = 31,
      .body = ua::BrowseRequest{
          .requested_max_references_per_node = 5,
          .nodes_to_browse = {{.node_id = NumericNode(40),
                               .browse_direction = ua::BrowseDirection::Forward,
                               .reference_type_id = NumericNode(41),
                               .include_subtypes = true}}}};
  ResponseMessage response{
      .request_handle = 31,
      .body = ua::BrowseResponse{
          .results = {{.status_code = Status{opcua::StatusCode::Good},
                       .continuation_point = {'q'},
                       .references = {{.reference_type_id = NumericNode(42),
                                       .is_forward = true,
                                       .node_id = opcua::ExpandedNodeId{
                                           NumericNode(43)}}}}}}};

  const auto decoded_request = *DecodeRequestMessage(EncodeJson(request));
  EXPECT_EQ(decoded_request.request_handle, request.request_handle);
  const auto* request_body =
      std::get_if<ua::BrowseRequest>(&decoded_request.body);
  ASSERT_NE(request_body, nullptr);
  EXPECT_EQ(request_body->requested_max_references_per_node, 5u);
  ASSERT_EQ(request_body->nodes_to_browse.size(), 1u);
  EXPECT_EQ(request_body->nodes_to_browse[0].node_id, NumericNode(40));
  EXPECT_EQ(request_body->nodes_to_browse[0].reference_type_id,
            NumericNode(41));

  const auto decoded_response = *DecodeResponseMessage(EncodeJson(response));
  EXPECT_EQ(decoded_response.request_handle, response.request_handle);
  const auto* response_body =
      std::get_if<ua::BrowseResponse>(&decoded_response.body);
  ASSERT_NE(response_body, nullptr);
  EXPECT_EQ(response_body->response_header.service_result.code(),
            opcua::StatusCode::Good);
  ASSERT_EQ(response_body->results.size(), 1u);
  EXPECT_EQ(response_body->results[0].continuation_point,
            (opcua::ByteString{'q'}));
  ASSERT_EQ(response_body->results[0].references.size(), 1u);
  EXPECT_EQ(response_body->results[0].references[0].node_id.node_id(),
            NumericNode(43));
}

TEST(JsonCodecTest, RoundTripsBrowseNextMessagesWithEnvelope) {
  RequestMessage request{
      .request_handle = 32,
      .body = ua::BrowseNextRequest{.release_continuation_points = false,
                                    .continuation_points = {{'a', 'b', 'c'}}}};
  ResponseMessage response{
      .request_handle = 32,
      .body = ua::BrowseNextResponse{
          .results = {{.status_code = Status{opcua::StatusCode::Good},
                       .references = {{.reference_type_id = NumericNode(52),
                                       .is_forward = false,
                                       .node_id = opcua::ExpandedNodeId{
                                           NumericNode(53)}}}}}}};

  const auto decoded_request = *DecodeRequestMessage(EncodeJson(request));
  const auto* request_body =
      std::get_if<ua::BrowseNextRequest>(&decoded_request.body);
  ASSERT_NE(request_body, nullptr);
  EXPECT_FALSE(request_body->release_continuation_points);
  EXPECT_EQ(request_body->continuation_points,
            (std::vector<opcua::ByteString>{{'a', 'b', 'c'}}));

  const auto decoded_response = *DecodeResponseMessage(EncodeJson(response));
  const auto* response_body =
      std::get_if<ua::BrowseNextResponse>(&decoded_response.body);
  ASSERT_NE(response_body, nullptr);
  EXPECT_EQ(response_body->response_header.service_result.code(),
            opcua::StatusCode::Good);
  ASSERT_EQ(response_body->results.size(), 1u);
  EXPECT_EQ(response_body->results[0].references[0].node_id.node_id(),
            NumericNode(53));
}

TEST(JsonCodecTest, RoundTripsSubscriptionLifecycleRequestMessages) {
  RequestMessage create_subscription{
      .request_handle = 41,
      .body = CreateSubscriptionRequest{
          .parameters = {.publishing_interval_ms = 1000,
                         .lifetime_count = 60,
                         .max_keep_alive_count = 10,
                         .max_notifications_per_publish = 0,
                         .publishing_enabled = true,
                         .priority = 1}}};
  RequestMessage modify_subscription{
      .request_handle = 42,
      .body = ModifySubscriptionRequest{
          .subscription_id = 17,
          .parameters = {.publishing_interval_ms = 250,
                         .lifetime_count = 30,
                         .max_keep_alive_count = 5,
                         .max_notifications_per_publish = 100,
                         .publishing_enabled = true,
                         .priority = 2}}};
  RequestMessage set_publishing_mode{
      .request_handle = 43,
      .body = ua::SetPublishingModeRequest{.publishing_enabled = false,
                                           .subscription_ids = {17, 18}}};
  RequestMessage delete_subscriptions{
      .request_handle = 44,
      .body = ua::DeleteSubscriptionsRequest{.subscription_ids = {19}}};

  const auto create_decoded =
      *DecodeRequestMessage(EncodeJson(create_subscription));
  EXPECT_EQ(create_decoded.request_handle, 41u);
  EXPECT_EQ(std::get<CreateSubscriptionRequest>(create_decoded.body)
                .parameters.max_keep_alive_count,
            10u);

  const auto modify_decoded =
      *DecodeRequestMessage(EncodeJson(modify_subscription));
  EXPECT_EQ(
      std::get<ModifySubscriptionRequest>(modify_decoded.body).subscription_id,
      17u);

  const auto set_mode_decoded =
      *DecodeRequestMessage(EncodeJson(set_publishing_mode));
  const auto& set_mode_body =
      std::get<ua::SetPublishingModeRequest>(set_mode_decoded.body);
  EXPECT_FALSE(set_mode_body.publishing_enabled);
  EXPECT_EQ(set_mode_body.subscription_ids,
            (std::vector<SubscriptionId>{17u, 18u}));

  const auto delete_decoded =
      *DecodeRequestMessage(EncodeJson(delete_subscriptions));
  EXPECT_EQ(std::get<ua::DeleteSubscriptionsRequest>(delete_decoded.body)
                .subscription_ids,
            (std::vector<UInt32>{19u}));
}

TEST(JsonCodecTest, RoundTripsMonitoredItemLifecycleMessages) {
  // A conformant EventFilter MonitoringFilter: select clauses + an OfType
  // where-clause (carried in the SCADA JSON blob as SelectClauses / of_type).
  // Unlike the old opaque-blob codec, the conformant codec reinterprets it
  // through the ua::EventFilter, so it round-trips canonically rather than
  // verbatim.
  const boost::json::value raw_event_filter = boost::json::parse(
      R"({"Type":"EventFilter","Body":{"SelectClauses":)"
      R"([{"BrowsePath":[{"Name":"Message"}]}]},)"
      R"("_scada":"event","types":0,"of_type":["i=100"],"child_of":[]})");
  RequestMessage create_items{
      .request_handle = 51,
      .body = CreateMonitoredItemsRequest{
          .subscription_id = 17,
          .timestamps_to_return = TimestampsToReturn::Both,
          .items_to_create = {
              {.item_to_monitor = {.node_id = NumericNode(70),
                                   .attribute_id = opcua::AttributeId::Value},
               .monitoring_mode = MonitoringMode::Reporting,
               .requested_parameters =
                   {.client_handle = 1,
                    .sampling_interval_ms = 250,
                    .filter = MonitoringFilter{DataChangeFilter{
                        .trigger = DataChangeTrigger::StatusValue,
                        .deadband_type = DeadbandType::Absolute,
                        .deadband_value = 0.5}},
                    .queue_size = 4,
                    .discard_oldest = true}},
              {.item_to_monitor = {.node_id = NumericNode(71),
                                   .attribute_id =
                                       opcua::AttributeId::EventNotifier},
               .index_range = "0:10",
               .monitoring_mode = MonitoringMode::Sampling,
               .requested_parameters = {
                   .client_handle = 2,
                   .sampling_interval_ms = 1000,
                   .filter = MonitoringFilter{raw_event_filter},
                   .queue_size = 1,
                   .discard_oldest = false}}}}};
  RequestMessage modify_items{
      .request_handle = 52,
      .body = ModifyMonitoredItemsRequest{
          .subscription_id = 17,
          .timestamps_to_return = TimestampsToReturn::Source,
          .items_to_modify = {
              {.monitored_item_id = 42,
               .requested_parameters = {.client_handle = 1,
                                        .sampling_interval_ms = 1000,
                                        .queue_size = 8,
                                        .discard_oldest = false}}}}};
  RequestMessage delete_items{
      .request_handle = 53,
      .body = ua::DeleteMonitoredItemsRequest{.subscription_id = 17,
                                              .monitored_item_ids = {42, 43}}};
  RequestMessage set_monitoring_mode{
      .request_handle = 54,
      .body = ua::SetMonitoringModeRequest{
          .subscription_id = 17,
          .monitoring_mode = ua::MonitoringMode::Disabled,
          .monitored_item_ids = {42}}};

  const auto create_decoded = *DecodeRequestMessage(EncodeJson(create_items));
  const auto& create_body =
      std::get<CreateMonitoredItemsRequest>(create_decoded.body);
  EXPECT_EQ(create_body.subscription_id, 17u);
  ASSERT_EQ(create_body.items_to_create.size(), 2u);
  EXPECT_EQ(create_body.items_to_create[0].requested_parameters.queue_size, 4u);
  ASSERT_TRUE(
      create_body.items_to_create[0].requested_parameters.filter.has_value());
  const auto* first_filter = std::get_if<DataChangeFilter>(
      &*create_body.items_to_create[0].requested_parameters.filter);
  ASSERT_NE(first_filter, nullptr);
  EXPECT_EQ(first_filter->deadband_value, 0.5);
  ASSERT_TRUE(
      create_body.items_to_create[1].requested_parameters.filter.has_value());
  const auto* second_filter = std::get_if<boost::json::value>(
      &*create_body.items_to_create[1].requested_parameters.filter);
  ASSERT_NE(second_filter, nullptr);
  EXPECT_EQ(*second_filter, raw_event_filter);

  const auto modify_decoded = *DecodeRequestMessage(EncodeJson(modify_items));
  EXPECT_EQ(std::get<ModifyMonitoredItemsRequest>(modify_decoded.body)
                .items_to_modify[0]
                .requested_parameters.sampling_interval_ms,
            1000);

  const auto delete_decoded = *DecodeRequestMessage(EncodeJson(delete_items));
  EXPECT_EQ(std::get<ua::DeleteMonitoredItemsRequest>(delete_decoded.body)
                .monitored_item_ids,
            (std::vector<UInt32>{42u, 43u}));

  const auto set_mode_decoded =
      *DecodeRequestMessage(EncodeJson(set_monitoring_mode));
  EXPECT_EQ(std::get<ua::SetMonitoringModeRequest>(set_mode_decoded.body)
                .monitoring_mode,
            ua::MonitoringMode::Disabled);
}

TEST(JsonCodecTest, RoundTripsSubscriptionLifecycleResponses) {
  const boost::json::value filter_result =
      boost::json::parse(R"({"Kind":"event","SelectClauseResults":[0]})");
  ResponseMessage create_subscription{
      .request_handle = 61,
      .body = CreateSubscriptionResponse{.status = opcua::StatusCode::Good,
                                         .subscription_id = 17,
                                         .revised_publishing_interval_ms = 1000,
                                         .revised_lifetime_count = 60,
                                         .revised_max_keep_alive_count = 10}};
  ResponseMessage modify_subscription{
      .request_handle = 62,
      .body = ModifySubscriptionResponse{.status = opcua::StatusCode::Good,
                                         .revised_publishing_interval_ms = 250,
                                         .revised_lifetime_count = 30,
                                         .revised_max_keep_alive_count = 5}};
  ua::SetPublishingModeResponse set_publishing_mode_response;
  set_publishing_mode_response.results = {
      Status{opcua::StatusCode::Good},
      Status{opcua::StatusCode::Bad_SubscriptionIdInvalid}};
  ResponseMessage set_publishing_mode{.request_handle = 63,
                                      .body = set_publishing_mode_response};
  ResponseMessage create_items{
      .request_handle = 64,
      .body = CreateMonitoredItemsResponse{
          .status = opcua::StatusCode::Good,
          .results = {{.status = opcua::StatusCode::Good,
                       .monitored_item_id = 42,
                       .revised_sampling_interval_ms = 250,
                       .revised_queue_size = 1},
                      {.status = opcua::StatusCode::Bad_NodeIdUnknown,
                       .monitored_item_id = 0,
                       .revised_sampling_interval_ms = 1000,
                       .revised_queue_size = 4,
                       .filter_result = filter_result}}}};
  ResponseMessage modify_items{
      .request_handle = 65,
      .body = ModifyMonitoredItemsResponse{
          .status = opcua::StatusCode::Good,
          .results = {{.status = opcua::StatusCode::Good,
                       .revised_sampling_interval_ms = 500,
                       .revised_queue_size = 8}}}};
  ua::DeleteMonitoredItemsResponse delete_items_response;
  delete_items_response.results = {Status{opcua::StatusCode::Good}};
  ResponseMessage delete_items{.request_handle = 66,
                               .body = delete_items_response};
  ua::SetMonitoringModeResponse set_monitoring_mode_response;
  set_monitoring_mode_response.results = {
      Status{opcua::StatusCode::Good},
      Status{opcua::StatusCode::Bad_SubscriptionIdInvalid}};
  ResponseMessage set_monitoring_mode{.request_handle = 67,
                                      .body = set_monitoring_mode_response};

  EXPECT_EQ(std::get<CreateSubscriptionResponse>(
                (*DecodeResponseMessage(EncodeJson(create_subscription))).body)
                .subscription_id,
            17u);
  EXPECT_EQ(std::get<ModifySubscriptionResponse>(
                (*DecodeResponseMessage(EncodeJson(modify_subscription))).body)
                .revised_max_keep_alive_count,
            5u);
  const ResponseMessage set_mode_decoded =
      *DecodeResponseMessage(EncodeJson(set_publishing_mode));
  const auto& set_mode_results =
      std::get<ua::SetPublishingModeResponse>(set_mode_decoded.body).results;
  ASSERT_EQ(set_mode_results.size(), 2u);
  EXPECT_TRUE(set_mode_results[0].good());
  EXPECT_EQ(set_mode_results[1].code(),
            opcua::StatusCode::Bad_SubscriptionIdInvalid);

  const auto encoded_create_items = EncodeJson(create_items);
  const auto& encoded_create_items_body =
      encoded_create_items.at("body").as_object();
  const auto& encoded_create_items_results =
      encoded_create_items_body.at("Results").as_array();
  ASSERT_EQ(encoded_create_items_results.size(), 2u);
  // The conformant compact encoding omits a Good (0) StatusCode; the second
  // result carries a Bad status, so its StatusCode field is present. Neither
  // uses the legacy "Status" field name.
  const auto& first_create_result = encoded_create_items_results[0].as_object();
  EXPECT_FALSE(first_create_result.if_contains("Status"));
  const auto& second_create_result =
      encoded_create_items_results[1].as_object();
  EXPECT_TRUE(second_create_result.if_contains("StatusCode"));
  EXPECT_FALSE(second_create_result.if_contains("Status"));

  const auto decoded_create_items =
      *DecodeResponseMessage(EncodeJson(create_items));
  const auto& create_items_body =
      std::get<CreateMonitoredItemsResponse>(decoded_create_items.body);
  ASSERT_EQ(create_items_body.results.size(), 2u);
  EXPECT_EQ(create_items_body.results[0].status.code(),
            opcua::StatusCode::Good);
  EXPECT_EQ(create_items_body.results[1].status.code(),
            opcua::StatusCode::Bad_NodeIdUnknown);
  ASSERT_TRUE(create_items_body.results[1].filter_result.has_value());
  EXPECT_EQ(*create_items_body.results[1].filter_result, filter_result);

  EXPECT_EQ(std::get<ModifyMonitoredItemsResponse>(
                (*DecodeResponseMessage(EncodeJson(modify_items))).body)
                .results[0]
                .revised_queue_size,
            8u);
  const ResponseMessage delete_items_decoded =
      *DecodeResponseMessage(EncodeJson(delete_items));
  const auto& delete_items_results =
      std::get<ua::DeleteMonitoredItemsResponse>(delete_items_decoded.body)
          .results;
  ASSERT_EQ(delete_items_results.size(), 1u);
  EXPECT_TRUE(delete_items_results[0].good());
  const ResponseMessage set_monitoring_decoded =
      *DecodeResponseMessage(EncodeJson(set_monitoring_mode));
  const auto& set_monitoring_results =
      std::get<ua::SetMonitoringModeResponse>(set_monitoring_decoded.body)
          .results;
  ASSERT_EQ(set_monitoring_results.size(), 2u);
  EXPECT_TRUE(set_monitoring_results[0].good());
  EXPECT_EQ(set_monitoring_results[1].code(),
            opcua::StatusCode::Bad_SubscriptionIdInvalid);
}

TEST(JsonCodecTest, RoundTripsPublishAndRecoveryRequestMessages) {
  RequestMessage publish{
      .request_handle = 71,
      .body =
          PublishRequest{.subscription_acknowledgements = {
                             {.subscription_id = 17, .sequence_number = 3},
                             {.subscription_id = 18, .sequence_number = 7}}}};
  RequestMessage republish{
      .request_handle = 72,
      .body = RepublishRequest{.subscription_id = 17,
                               .retransmit_sequence_number = 5}};
  RequestMessage transfer{
      .request_handle = 73,
      .body = ua::TransferSubscriptionsRequest{.subscription_ids = {17, 18},
                                               .send_initial_values = true}};
  const std::vector<SubscriptionAcknowledgement> expected_acks{
      {.subscription_id = 17, .sequence_number = 3},
      {.subscription_id = 18, .sequence_number = 7}};
  const std::vector<SubscriptionId> expected_transfer_ids{17u, 18u};

  const auto publish_decoded = *DecodeRequestMessage(EncodeJson(publish));
  EXPECT_EQ(publish_decoded.request_handle, 71u);
  EXPECT_EQ(std::get<PublishRequest>(publish_decoded.body)
                .subscription_acknowledgements,
            expected_acks);

  const auto republish_decoded = *DecodeRequestMessage(EncodeJson(republish));
  EXPECT_EQ(std::get<RepublishRequest>(republish_decoded.body)
                .retransmit_sequence_number,
            5u);

  const auto transfer_decoded = *DecodeRequestMessage(EncodeJson(transfer));
  const auto& transfer_body =
      std::get<ua::TransferSubscriptionsRequest>(transfer_decoded.body);
  EXPECT_EQ(transfer_body.subscription_ids, expected_transfer_ids);
  EXPECT_TRUE(transfer_body.send_initial_values);
}

TEST(JsonCodecTest, RoundTripsPublishAndRecoveryResponses) {
  const auto publish_time = ParseTime("2026-04-19 00:00:05");
  opcua::DataValue republish_value;
  republish_value.value = opcua::Variant{true};
  NotificationMessage publish_message{
      .sequence_number = 3,
      .publish_time = publish_time,
      .notification_data = {
          DataChangeNotification{
              .monitored_items = {{.client_handle = 1,
                                   .value =
                                       opcua::DataValue{
                                           opcua::Variant{42.5},
                                           opcua::Qualifier{
                                               opcua::Qualifier::MANUAL},
                                           ParseTime("2026-04-19 00:00:05"),
                                           ParseTime("2026-04-19 00:00:06")}}}},
          EventNotificationList{
              .events =
                  {{.client_handle = 2,
                    .event_fields = {opcua::Variant{std::string{"AlarmRaised"}},
                                     opcua::Variant{opcua::UInt32{500}}}}}},
          StatusChangeNotification{.status = opcua::StatusCode::Bad_Timeout}}};
  ResponseMessage publish{
      .request_handle = 81,
      .body = PublishResponse{.status = opcua::StatusCode::Good,
                              .subscription_id = 17,
                              .results = {opcua::StatusCode::Good},
                              .more_notifications = true,
                              .notification_message = publish_message,
                              .available_sequence_numbers = {3, 4}}};
  ResponseMessage republish{
      .request_handle = 82,
      .body = RepublishResponse{
          .status = opcua::StatusCode::Good,
          .notification_message = {
              .sequence_number = 5,
              .publish_time = ParseTime("2026-04-19 00:00:07"),
              .notification_data = {DataChangeNotification{
                  .monitored_items = {
                      {.client_handle = 9, .value = republish_value}}}}}}};
  ua::TransferSubscriptionsResponse transfer_response;
  transfer_response.results = {
      ua::TransferResult{.status_code = Status{opcua::StatusCode::Good}},
      ua::TransferResult{
          .status_code = Status{opcua::StatusCode::Bad_SubscriptionIdInvalid}}};
  ResponseMessage transfer{.request_handle = 83, .body = transfer_response};

  std::optional<boost::json::value> publish_json;
  ASSERT_NO_THROW(publish_json.emplace(EncodeJson(publish)));
  const auto& notification_json = publish_json->as_object()
                                      .at("body")
                                      .as_object()
                                      .at("NotificationMessage")
                                      .as_object();
  EXPECT_EQ(notification_json.at("PublishTime").as_string(),
            "2026-04-19T00:00:05Z");
  // The NotificationData rides as an INLINE JSON ExtensionObject body (UaBody
  // is an object), not a UaEncoding=1 base64 binary body — the web client has
  // no binary decoder and must read the notification fields directly.
  const auto& first_notification =
      notification_json.at("NotificationData").as_array().at(0).as_object();
  EXPECT_FALSE(first_notification.contains("UaEncoding"));
  ASSERT_TRUE(first_notification.at("UaBody").is_object());
  EXPECT_TRUE(
      first_notification.at("UaBody").as_object().contains("MonitoredItems"));
  std::optional<ResponseMessage> decoded_publish;
  ASSERT_NO_THROW(
      decoded_publish.emplace(*DecodeResponseMessage(*publish_json)));
  EXPECT_EQ(decoded_publish->request_handle, 81u);
  const auto& publish_body = std::get<PublishResponse>(decoded_publish->body);
  EXPECT_EQ(publish_body.subscription_id, 17u);
  EXPECT_EQ(publish_body.available_sequence_numbers,
            (std::vector<opcua::UInt32>{3u, 4u}));
  EXPECT_TRUE(publish_body.more_notifications);
  EXPECT_EQ(publish_body.notification_message.sequence_number, 3u);
  EXPECT_EQ(publish_body.notification_message.publish_time, publish_time);
  ASSERT_EQ(publish_body.notification_message.notification_data.size(), 3u);
  const auto* data_change = std::get_if<DataChangeNotification>(
      &publish_body.notification_message.notification_data[0]);
  ASSERT_NE(data_change, nullptr);
  ASSERT_EQ(data_change->monitored_items.size(), 1u);
  EXPECT_EQ(data_change->monitored_items[0].client_handle, 1u);
  EXPECT_EQ(data_change->monitored_items[0].value.value.get<double>(), 42.5);
  const auto* events = std::get_if<EventNotificationList>(
      &publish_body.notification_message.notification_data[1]);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(events->events.size(), 1u);
  EXPECT_EQ(events->events[0].client_handle, 2u);
  EXPECT_EQ(events->events[0].event_fields.size(), 2u);
  const auto* status_change = std::get_if<StatusChangeNotification>(
      &publish_body.notification_message.notification_data[2]);
  ASSERT_NE(status_change, nullptr);
  EXPECT_EQ(status_change->status, opcua::StatusCode::Bad_Timeout);
  EXPECT_EQ(publish_body.results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));

  std::optional<boost::json::value> republish_json;
  ASSERT_NO_THROW(republish_json.emplace(EncodeJson(republish)));
  std::optional<ResponseMessage> decoded_republish;
  ASSERT_NO_THROW(
      decoded_republish.emplace(*DecodeResponseMessage(*republish_json)));
  EXPECT_EQ(std::get<RepublishResponse>(decoded_republish->body)
                .notification_message.sequence_number,
            5u);

  std::optional<boost::json::value> transfer_json;
  ASSERT_NO_THROW(transfer_json.emplace(EncodeJson(transfer)));
  std::optional<ResponseMessage> decoded_transfer;
  ASSERT_NO_THROW(
      decoded_transfer.emplace(*DecodeResponseMessage(*transfer_json)));
  const auto& transfer_results =
      std::get<ua::TransferSubscriptionsResponse>(decoded_transfer->body)
          .results;
  ASSERT_EQ(transfer_results.size(), 2u);
  EXPECT_TRUE(transfer_results[0].status_code.good());
  EXPECT_EQ(transfer_results[1].status_code.code(),
            opcua::StatusCode::Bad_SubscriptionIdInvalid);
}

TEST(JsonCodecTest, RoundTripsCallAndMutationResponses) {
  ua::CallResponse call{
      .results = {{.status_code = opcua::StatusCode::Good},
                  {.status_code = opcua::StatusCode::Bad_InvalidArgument}}};
  ua::AddNodesResponse add_nodes;
  add_nodes.results = {
      ua::AddNodesResult{.status_code = Status{opcua::StatusCode::Good},
                         .added_node_id = NumericNode(300)}};
  ua::DeleteNodesResponse delete_nodes;
  delete_nodes.response_header.service_result =
      Status{opcua::StatusCode::Bad_NoCommunication};
  delete_nodes.results = {Status{opcua::StatusCode::Bad_NoCommunication}};
  ua::AddReferencesResponse add_refs;
  add_refs.results = {Status{opcua::StatusCode::Good},
                      Status{opcua::StatusCode::Bad_TargetNodeIdInvalid}};
  ua::DeleteReferencesResponse delete_refs;
  delete_refs.results = {Status{opcua::StatusCode::Good}};

  const auto decoded_call = std::get<ua::CallResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{call})));
  ASSERT_EQ(decoded_call.results.size(), call.results.size());
  EXPECT_EQ(decoded_call.results[0].status_code.code(),
            call.results[0].status_code.code());
  EXPECT_EQ(decoded_call.results[1].status_code.code(),
            call.results[1].status_code.code());
  const auto decoded_add_nodes = std::get<ua::AddNodesResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{add_nodes})));
  ASSERT_EQ(decoded_add_nodes.results.size(), 1u);
  EXPECT_EQ(decoded_add_nodes.results[0].status_code.code(),
            add_nodes.results[0].status_code.code());
  EXPECT_EQ(decoded_add_nodes.results[0].added_node_id,
            add_nodes.results[0].added_node_id);
  const auto decoded_delete_nodes = std::get<ua::DeleteNodesResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{delete_nodes})));
  ASSERT_EQ(decoded_delete_nodes.results.size(), 1u);
  EXPECT_EQ(decoded_delete_nodes.results[0].code(),
            opcua::StatusCode::Bad_NoCommunication);
  const auto decoded_add_refs = std::get<ua::AddReferencesResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{add_refs})));
  ASSERT_EQ(decoded_add_refs.results.size(), 2u);
  EXPECT_EQ(decoded_add_refs.results[0].code(), opcua::StatusCode::Good);
  EXPECT_EQ(decoded_add_refs.results[1].code(),
            opcua::StatusCode::Bad_TargetNodeIdInvalid);
  const auto decoded_delete_refs = std::get<ua::DeleteReferencesResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{delete_refs})));
  ASSERT_EQ(decoded_delete_refs.results.size(), 1u);
  EXPECT_EQ(decoded_delete_refs.results[0].code(), opcua::StatusCode::Good);
}

TEST(JsonCodecTest, RejectsUnknownService) {
  boost::json::value json = boost::json::object{
      {"service", "Unknown"}, {"body", boost::json::object{}}};
  EXPECT_EQ(DecodeServiceRequest(json).status().code(),
            opcua::StatusCode::Bad_TypeMismatch);
  EXPECT_EQ(DecodeServiceResponse(json).status().code(),
            opcua::StatusCode::Bad_TypeMismatch);
}

}  // namespace
}  // namespace opcua::ws
