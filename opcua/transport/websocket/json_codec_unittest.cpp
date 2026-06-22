#include "opcua/transport/websocket/json_codec.h"

#include "opcua/base/time_utils.h"
#include "opcua/events/event.h"

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

opcua::base::Time ParseTime(std::string_view value) {
  opcua::base::Time result;
  EXPECT_TRUE(Deserialize(value, result));
  return result;
}

// Pin the spec wire shape (Part 6 §5.4.2.10–17) for the primitive codecs by
// asserting on the literal JSON the codec emits when wrapping each primitive
// inside a Read response (the smallest envelope that carries DataValue and
// Variant). Future regressions surface here without needing the full e2e.
TEST(JsonCodecTest, ReadResponseWireShapeMatchesSpec) {
  ReadResponse read{
      .status = opcua::StatusCode::Good,
      .results = {opcua::DataValue{opcua::Variant{opcua::Int32{42}},
                                   opcua::Qualifier{opcua::Qualifier::MANUAL},
                                   ParseTime("2026-04-19 10:00:00"),
                                   ParseTime("2026-04-19 10:00:01")}}};
  // Force a non-default status_code so it lands on the wire.
  read.results[0].status_code = opcua::StatusCode::Bad_Disconnected;

  const auto encoded = EncodeJson(ServiceResponse{read});
  ASSERT_TRUE(encoded.is_object());
  const auto& body = encoded.as_object().at("body").as_object();
  ASSERT_TRUE(body.contains("Status"));
  ASSERT_TRUE(body.contains("Results"));
  EXPECT_TRUE(body.at("Status").is_uint64());
  EXPECT_EQ(body.at("Status").as_uint64(), 0u);
  const auto& dv = body.at("Results").as_array().at(0).as_object();

  // §5.4.2.17: Status (not StatusCode), Source/ServerTimestamp present;
  // the scada-specific Qualifier never appears on the wire.
  EXPECT_TRUE(dv.contains("Status"));
  EXPECT_FALSE(dv.contains("StatusCode"));
  EXPECT_FALSE(dv.contains("Qualifier"));
  EXPECT_TRUE(dv.contains("SourceTimestamp"));
  EXPECT_TRUE(dv.contains("ServerTimestamp"));
  EXPECT_EQ(dv.at("SourceTimestamp").as_string(), "2026-04-19T10:00:00Z");
  EXPECT_EQ(dv.at("ServerTimestamp").as_string(), "2026-04-19T10:00:01Z");

  // §5.4.2.16: Variant carries numeric Type id (Int32 == 6) and Body — no
  // IsArray flag.
  const auto& variant = dv.at("Value").as_object();
  EXPECT_EQ(variant.at("Type").to_number<int>(), 6);
  EXPECT_EQ(variant.at("Body").to_number<int>(), 42);
  EXPECT_FALSE(variant.contains("IsArray"));

  // Round-trip preserves Variant value, Status and timestamps. Qualifier
  // is intentionally dropped (no spec field), so the decoded qualifier
  // returns to default — assert per-field rather than via vector equality.
  const auto decoded = std::get<ReadResponse>(*DecodeServiceResponse(encoded));
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
  BrowseResponse browse{
      .status = opcua::StatusCode::Good,
      .results = {
          {.status_code = opcua::StatusCode::Good,
           .references = {{.reference_type_id = NumericNode(301),
                           .forward = false,
                           .node_id = NumericNode(302),
                           .node_class = opcua::NodeClass::Variable}}}}};

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

  // §5.4.2.10: NodeId / ReferenceTypeId on a Reference are JSON strings.
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
      std::get<BrowseResponse>(*DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  ASSERT_EQ(decoded.results[0].references.size(), 1u);
  EXPECT_EQ(decoded.results[0].references[0].node_id, NumericNode(302));
  EXPECT_FALSE(decoded.results[0].references[0].forward);
  EXPECT_EQ(decoded.results[0].references[0].node_class,
            opcua::NodeClass::Variable);
}

TEST(JsonCodecTest, TranslateBrowsePathsWireShapeMatchesSpec) {
  TranslateBrowsePathsResponse translate{
      .status = opcua::StatusCode::Good,
      .results = {
          {.status_code = opcua::StatusCode::Good,
           .targets = {{.target_id = opcua::ExpandedNodeId{NumericNode(303),
                                                           "urn:test", 2},
                        .remaining_path_index = 1}}}}};

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

  const auto decoded =
      std::get<TranslateBrowsePathsResponse>(*DecodeServiceResponse(encoded));
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
  ReadResponse read{.status = opcua::StatusCode::Good,
                    .results = {opcua::DataValue{}}};
  const auto encoded = EncodeJson(ServiceResponse{read});
  const auto& dv = encoded.as_object()
                       .at("body")
                       .as_object()
                       .at("Results")
                       .as_array()
                       .at(0)
                       .as_object();
  // No Value, no Status (Good is default), no timestamps — all elided.
  EXPECT_TRUE(dv.empty());
}

TEST(JsonCodecTest, RoundTripsPhase0Requests) {
  ReadRequest read{
      .inputs = {{.node_id = NumericNode(1),
                  .attribute_id = opcua::AttributeId::DisplayName}}};
  WriteRequest write{
      .inputs = {{.node_id = NumericNode(2),
                  .attribute_id = opcua::AttributeId::Value,
                  .value = opcua::Variant{std::vector<opcua::UInt32>{4, 5}},
                  .flags = opcua::WriteFlags{}.set_select().set_param()}}};
  BrowseRequest browse{.requested_max_references_per_node = 7,
                       .inputs = {{.node_id = NumericNode(3),
                                   .direction = opcua::BrowseDirection::Inverse,
                                   .reference_type_id = NumericNode(31),
                                   .include_subtypes = false}}};
  BrowseNextRequest browse_next{
      .release_continuation_points = true,
      .continuation_points = {{'a', 'b'}, {'x', 'y', 'z'}}};
  TranslateBrowsePathsRequest translate{
      .inputs = {{.node_id = NumericNode(4),
                  .relative_path = {{.reference_type_id = NumericNode(41),
                                     .inverse = true,
                                     .include_subtypes = false,
                                     .target_name = {"Child", 6}}}}}};

  const auto decoded_read = std::get<ReadRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{read})));
  ASSERT_EQ(decoded_read.inputs.size(), 1u);
  EXPECT_EQ(decoded_read.inputs[0], read.inputs[0]);

  const auto decoded_write = std::get<WriteRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{write})));
  ASSERT_EQ(decoded_write.inputs.size(), 1u);
  EXPECT_EQ(decoded_write.inputs[0], write.inputs[0]);

  const auto decoded_browse = std::get<BrowseRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{browse})));
  EXPECT_EQ(decoded_browse.requested_max_references_per_node,
            browse.requested_max_references_per_node);
  ASSERT_EQ(decoded_browse.inputs.size(), 1u);
  EXPECT_EQ(decoded_browse.inputs[0], browse.inputs[0]);

  const auto decoded_browse_next = std::get<BrowseNextRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{browse_next})));
  EXPECT_TRUE(decoded_browse_next.release_continuation_points);
  EXPECT_EQ(decoded_browse_next.continuation_points,
            browse_next.continuation_points);

  const auto decoded_translate = std::get<TranslateBrowsePathsRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{translate})));
  ASSERT_EQ(decoded_translate.inputs.size(), 1u);
  EXPECT_EQ(decoded_translate.inputs[0], translate.inputs[0]);
}

TEST(JsonCodecTest, RoundTripsCanonicalEnvelopeTypes) {
  const RequestMessage request{
      .request_handle = 91,
      .body = ReadRequest{
          .inputs = {{.node_id = NumericNode(9),
                      .attribute_id = opcua::AttributeId::BrowseName}}}};
  const auto decoded_request = *DecodeRequestMessage(EncodeJson(request));
  EXPECT_EQ(decoded_request.request_handle, request.request_handle);
  const auto* decoded_read = std::get_if<ReadRequest>(&decoded_request.body);
  ASSERT_NE(decoded_read, nullptr);
  ASSERT_EQ(decoded_read->inputs.size(), 1u);
  EXPECT_EQ(decoded_read->inputs[0],
            std::get<ReadRequest>(request.body).inputs[0]);

  const ResponseMessage response{
      .request_handle = 92,
      .body = ServiceFault{.status = opcua::StatusCode::Bad_CantParseString}};
  const auto decoded_response = *DecodeResponseMessage(EncodeJson(response));
  EXPECT_EQ(decoded_response.request_handle, response.request_handle);
  const auto* decoded_fault = std::get_if<ServiceFault>(&decoded_response.body);
  ASSERT_NE(decoded_fault, nullptr);
  EXPECT_EQ(decoded_fault->status.code(),
            opcua::StatusCode::Bad_CantParseString);
}

TEST(JsonCodecTest, RequestWireShapeUsesSpecFieldNames) {
  const auto read_json = EncodeJson(ServiceRequest{
      ReadRequest{.inputs = {{.node_id = NumericNode(1),
                              .attribute_id = opcua::AttributeId::Value}}}});
  const auto write_json = EncodeJson(ServiceRequest{
      WriteRequest{.inputs = {{.node_id = NumericNode(2),
                               .attribute_id = opcua::AttributeId::Value,
                               .value = opcua::Variant{opcua::Int32{7}},
                               .flags = {}}}}});
  const auto browse_json = EncodeJson(ServiceRequest{
      BrowseRequest{.requested_max_references_per_node = 5,
                    .inputs = {{.node_id = NumericNode(3),
                                .direction = opcua::BrowseDirection::Forward,
                                .reference_type_id = NumericNode(31),
                                .include_subtypes = true}}}});
  const auto translate_json =
      EncodeJson(ServiceRequest{TranslateBrowsePathsRequest{
          .inputs = {{.node_id = NumericNode(4),
                      .relative_path = {{.reference_type_id = NumericNode(41),
                                         .inverse = false,
                                         .include_subtypes = true,
                                         .target_name = {"Child", 1}}}}}}});

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

TEST(JsonCodecTest, DecodeWriteRequestAcceptsLegacyDataValueWrapper) {
  const auto request = *DecodeServiceRequest(boost::json::parse(R"json(
{
  "service": "Write",
  "body": {
    "NodesToWrite": [
      {
        "NodeId": "ns=2;s=UserProfile",
        "AttributeId": 13,
        "Value": {
          "Value": {
            "Type": 12,
            "Body": "{\"version\":1,\"favorites\":[\"ns=2;i=1001\"]}"
          }
        },
        "Flags": 0
      }
    ]
  }
}
)json"));

  const auto* write = std::get_if<WriteRequest>(&request);
  ASSERT_NE(write, nullptr);
  ASSERT_EQ(write->inputs.size(), 1u);
  EXPECT_EQ(write->inputs[0].node_id,
            opcua::NodeId::FromString("ns=2;s=UserProfile"));
  EXPECT_EQ(write->inputs[0].attribute_id, opcua::AttributeId::Value);
  EXPECT_EQ(write->inputs[0].value,
            opcua::Variant{opcua::String{
                "{\"version\":1,\"favorites\":[\"ns=2;i=1001\"]}"}});
}

TEST(JsonCodecTest, RoundTripsSessionRequestMessages) {
  RequestMessage create{
      .request_handle = 11,
      .body = CreateSessionRequest{
          .requested_timeout = opcua::base::TimeDelta::FromSeconds(45)}};
  RequestMessage activate{.request_handle = 12,
                          .body = ActivateSessionRequest{
                              .session_id = NumericNode(20),
                              .authentication_token = NumericNode(21, 3),
                              .user_name = opcua::LocalizedText{u"operator"},
                              .password = opcua::LocalizedText{u"secret"},
                              .delete_existing = true,
                              .allow_anonymous = false,
                          }};
  RequestMessage close{
      .request_handle = 13,
      .body = CloseSessionRequest{.session_id = NumericNode(22),
                                  .authentication_token = NumericNode(23, 3)}};

  const auto decoded_create = *DecodeRequestMessage(EncodeJson(create));
  EXPECT_EQ(decoded_create.request_handle, create.request_handle);
  const auto* create_body =
      std::get_if<CreateSessionRequest>(&decoded_create.body);
  ASSERT_NE(create_body, nullptr);
  EXPECT_EQ(create_body->requested_timeout,
            opcua::base::TimeDelta::FromSeconds(45));

  const auto decoded_activate = *DecodeRequestMessage(EncodeJson(activate));
  EXPECT_EQ(decoded_activate.request_handle, activate.request_handle);
  const auto* activate_body =
      std::get_if<ActivateSessionRequest>(&decoded_activate.body);
  ASSERT_NE(activate_body, nullptr);
  EXPECT_EQ(activate_body->session_id, NumericNode(20));
  EXPECT_EQ(activate_body->authentication_token, NumericNode(21, 3));
  ASSERT_TRUE(activate_body->user_name.has_value());
  ASSERT_TRUE(activate_body->password.has_value());
  EXPECT_EQ(*activate_body->user_name, opcua::LocalizedText{u"operator"});
  EXPECT_EQ(*activate_body->password, opcua::LocalizedText{u"secret"});
  EXPECT_TRUE(activate_body->delete_existing);
  EXPECT_FALSE(activate_body->allow_anonymous);

  const auto decoded_close = *DecodeRequestMessage(EncodeJson(close));
  EXPECT_EQ(decoded_close.request_handle, close.request_handle);
  const auto* close_body =
      std::get_if<CloseSessionRequest>(&decoded_close.body);
  ASSERT_NE(close_body, nullptr);
  EXPECT_EQ(close_body->session_id, NumericNode(22));
  EXPECT_EQ(close_body->authentication_token, NumericNode(23, 3));
}

TEST(JsonCodecTest, EncodesAndDecodesPascalCaseSessionMessageFields) {
  const auto create_json = EncodeJson(RequestMessage{
      .request_handle = 11,
      .body = CreateSessionRequest{
          .requested_timeout = opcua::base::TimeDelta::FromSeconds(45)}});
  const auto create_text = boost::json::serialize(create_json);
  EXPECT_NE(create_text.find("\"RequestedSessionTimeout\""), std::string::npos);
  EXPECT_EQ(create_text.find("\"requestedTimeoutMs\""), std::string::npos);

  const auto decoded_create = *DecodeRequestMessage(boost::json::parse(
      R"({"requestHandle":11,"service":"CreateSession","body":{"RequestedSessionTimeout":45000}})"));
  const auto* create_body =
      std::get_if<CreateSessionRequest>(&decoded_create.body);
  ASSERT_NE(create_body, nullptr);
  EXPECT_EQ(create_body->requested_timeout,
            opcua::base::TimeDelta::FromSeconds(45));

  const auto activate_json = EncodeJson(
      RequestMessage{.request_handle = 12,
                     .body = ActivateSessionRequest{
                         .session_id = NumericNode(20),
                         .authentication_token = NumericNode(21, 3),
                         .user_name = opcua::LocalizedText{u"operator"},
                         .password = opcua::LocalizedText{u"secret"},
                         .delete_existing = true,
                         .allow_anonymous = false,
                     }});
  const auto activate_text = boost::json::serialize(activate_json);
  EXPECT_NE(activate_text.find("\"SessionId\""), std::string::npos);
  EXPECT_NE(activate_text.find("\"AuthenticationToken\""), std::string::npos);
  EXPECT_NE(activate_text.find("\"UserName\""), std::string::npos);
  EXPECT_NE(activate_text.find("\"Password\""), std::string::npos);
  EXPECT_EQ(activate_text.find("\"sessionId\""), std::string::npos);
  EXPECT_EQ(activate_text.find("\"authenticationToken\""), std::string::npos);
}

TEST(JsonCodecTest, RoundTripsHistoryReadRawRequest) {
  HistoryReadRawRequest request{
      .details = {
          .node_id = NumericNode(1),
          .from = ParseTime("2026-04-19 10:00:00"),
          .to = ParseTime("2026-04-19 12:00:00"),
          .max_count = 123,
          .aggregation = {.start_time = ParseTime("2026-04-19 09:30:00"),
                          .interval = opcua::base::TimeDelta::FromMinutes(15),
                          .aggregate_type = NumericNode(44)},
          .release_continuation_point = true,
          .continuation_point = {'a', 'b', 'c'}}};

  auto json = EncodeJson(ServiceRequest{request});
  auto decoded = *DecodeServiceRequest(json);
  EXPECT_NE(boost::json::serialize(json).find("HistoryReadRaw"),
            std::string::npos);
  const auto* typed = std::get_if<HistoryReadRawRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->details.node_id, request.details.node_id);
  EXPECT_EQ(typed->details.from, request.details.from);
  EXPECT_EQ(typed->details.to, request.details.to);
  EXPECT_EQ(typed->details.max_count, request.details.max_count);
  EXPECT_EQ(typed->details.aggregation, request.details.aggregation);
  EXPECT_EQ(typed->details.release_continuation_point,
            request.details.release_continuation_point);
  EXPECT_EQ(typed->details.continuation_point,
            request.details.continuation_point);
}

TEST(JsonCodecTest, RoundTripsHistoryReadRawRequestMessage) {
  RequestMessage request{.request_handle = 8,
                         .body = HistoryReadRawRequest{
                             .details = {.node_id = NumericNode(101, 1)}}};

  const auto encoded = boost::json::serialize(EncodeJson(request));
  const auto decoded = *DecodeRequestMessage(boost::json::parse(encoded));

  EXPECT_EQ(decoded.request_handle, request.request_handle);
  const auto* typed = std::get_if<HistoryReadRawRequest>(&decoded.body);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->details.node_id, NumericNode(101, 1));
}

TEST(JsonCodecTest, RoundTripsHistoryReadEventsRequest) {
  HistoryReadEventsRequest request{
      .details = {.node_id = NumericNode(2),
                  .from = ParseTime("2026-04-19 08:00:00"),
                  .to = ParseTime("2026-04-19 09:00:00"),
                  .filter = {.types = opcua::EventFilter::UNACKED,
                             .of_type = {NumericNode(77), NumericNode(78)},
                             .child_of = {NumericNode(79)}}}};

  auto decoded = *DecodeServiceRequest(EncodeJson(ServiceRequest{request}));
  const auto* typed = std::get_if<HistoryReadEventsRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->details.node_id, request.details.node_id);
  EXPECT_EQ(typed->details.from, request.details.from);
  EXPECT_EQ(typed->details.to, request.details.to);
  EXPECT_EQ(typed->details.filter, request.details.filter);
}

TEST(JsonCodecTest, RoundTripsCallRequestWithScalarAndArrayVariants) {
  CallRequest request{
      .methods = {
          {.object_id = NumericNode(10),
           .method_id = NumericNode(11),
           .arguments = {opcua::Variant{true},
                         opcua::Variant{std::vector<opcua::Int32>{1, 2, 3}},
                         opcua::Variant{opcua::LocalizedText{u"hello"}},
                         opcua::Variant{NumericNode(12)}}}}};

  auto decoded = *DecodeServiceRequest(EncodeJson(ServiceRequest{request}));
  const auto* typed = std::get_if<CallRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->methods.size(), 1u);
  EXPECT_EQ(typed->methods[0].object_id, request.methods[0].object_id);
  EXPECT_EQ(typed->methods[0].method_id, request.methods[0].method_id);
  EXPECT_EQ(typed->methods[0].arguments, request.methods[0].arguments);
}

TEST(JsonCodecTest, CallWireShapeUsesSpecFieldNames) {
  const auto json = EncodeJson(ServiceRequest{CallRequest{
      .methods = {{.object_id = NumericNode(10),
                   .method_id = NumericNode(11),
                   .arguments = {opcua::Variant{opcua::Int32{5}}}}}}});

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

  CallRequest request{
      .methods = {
          {.object_id = NumericNode(120),
           .method_id = NumericNode(121),
           .arguments = {
               opcua::Variant{opcua::ExtensionObject{
                   opcua::ExpandedNodeId{NumericNode(122), "urn:test", 3},
                   scalar_payload}},
               opcua::Variant{std::vector<opcua::ExtensionObject>{
                   {opcua::ExpandedNodeId{NumericNode(123)}, array_payload_1},
                   {opcua::ExpandedNodeId{NumericNode(124), "urn:test", 4},
                    array_payload_2}}}}}}};

  const auto decoded =
      *DecodeServiceRequest(EncodeJson(ServiceRequest{request}));
  const auto* typed = std::get_if<CallRequest>(&decoded);
  ASSERT_NE(typed, nullptr);
  ASSERT_EQ(typed->methods.size(), 1u);
  ASSERT_EQ(typed->methods[0].arguments.size(), 2u);

  const auto& scalar_extension =
      typed->methods[0].arguments[0].get<opcua::ExtensionObject>();
  EXPECT_EQ(scalar_extension.data_type_id(),
            (opcua::ExpandedNodeId{NumericNode(122), "urn:test", 3}));
  const auto* scalar_decoded =
      std::any_cast<boost::json::value>(&scalar_extension.value());
  ASSERT_NE(scalar_decoded, nullptr);
  EXPECT_EQ(*scalar_decoded, scalar_payload);

  const auto& array_extensions =
      typed->methods[0].arguments[1].get<std::vector<opcua::ExtensionObject>>();
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
  AddNodesRequest add_nodes{
      .items = {{.requested_id = NumericNode(100),
                 .parent_id = NumericNode(101),
                 .node_class = opcua::NodeClass::Variable,
                 .type_definition_id = NumericNode(102),
                 .attributes = opcua::NodeAttributes{}
                                   .set_browse_name({"Pressure", 3})
                                   .set_display_name(u"Pressure")
                                   .set_data_type(NumericNode(103))
                                   .set_value(opcua::Variant{42.5})}}};
  DeleteNodesRequest delete_nodes{
      .items = {
          {.node_id = NumericNode(104), .delete_target_references = true}}};
  AddReferencesRequest add_refs{
      .items = {{.source_node_id = NumericNode(105),
                 .reference_type_id = NumericNode(106),
                 .forward = false,
                 .target_server_uri = "opc.tcp://server",
                 .target_node_id =
                     opcua::ExpandedNodeId{NumericNode(107), "urn:test", 2},
                 .target_node_class = opcua::NodeClass::Object}}};
  DeleteReferencesRequest delete_refs{
      .items = {{.source_node_id = NumericNode(108),
                 .reference_type_id = NumericNode(109),
                 .forward = true,
                 .target_node_id = opcua::ExpandedNodeId{NumericNode(110)},
                 .delete_bidirectional = false}}};

  const auto decoded_add_nodes = std::get<AddNodesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{add_nodes})));
  ASSERT_EQ(decoded_add_nodes.items.size(), 1u);
  EXPECT_EQ(decoded_add_nodes.items[0], add_nodes.items[0]);

  const auto decoded_delete_nodes = std::get<DeleteNodesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{delete_nodes})));
  ASSERT_EQ(decoded_delete_nodes.items.size(), 1u);
  EXPECT_EQ(decoded_delete_nodes.items[0].node_id,
            delete_nodes.items[0].node_id);
  EXPECT_EQ(decoded_delete_nodes.items[0].delete_target_references,
            delete_nodes.items[0].delete_target_references);

  const auto decoded_add_refs = std::get<AddReferencesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{add_refs})));
  ASSERT_EQ(decoded_add_refs.items.size(), 1u);
  EXPECT_EQ(decoded_add_refs.items[0].source_node_id,
            add_refs.items[0].source_node_id);
  EXPECT_EQ(decoded_add_refs.items[0].reference_type_id,
            add_refs.items[0].reference_type_id);
  EXPECT_EQ(decoded_add_refs.items[0].forward, add_refs.items[0].forward);
  EXPECT_EQ(decoded_add_refs.items[0].target_server_uri,
            add_refs.items[0].target_server_uri);
  EXPECT_EQ(decoded_add_refs.items[0].target_node_id,
            add_refs.items[0].target_node_id);
  EXPECT_EQ(decoded_add_refs.items[0].target_node_class,
            add_refs.items[0].target_node_class);

  const auto decoded_delete_refs = std::get<DeleteReferencesRequest>(
      *DecodeServiceRequest(EncodeJson(ServiceRequest{delete_refs})));
  ASSERT_EQ(decoded_delete_refs.items.size(), 1u);
  EXPECT_EQ(decoded_delete_refs.items[0].source_node_id,
            delete_refs.items[0].source_node_id);
  EXPECT_EQ(decoded_delete_refs.items[0].reference_type_id,
            delete_refs.items[0].reference_type_id);
  EXPECT_EQ(decoded_delete_refs.items[0].forward, delete_refs.items[0].forward);
  EXPECT_EQ(decoded_delete_refs.items[0].target_node_id,
            delete_refs.items[0].target_node_id);
  EXPECT_EQ(decoded_delete_refs.items[0].delete_bidirectional,
            delete_refs.items[0].delete_bidirectional);
}

TEST(JsonCodecTest, NodeManagementWireShapeUsesSpecFieldNames) {
  const auto add_nodes = EncodeJson(ServiceRequest{
      AddNodesRequest{.items = {{.requested_id = NumericNode(100),
                                 .parent_id = NumericNode(101),
                                 .node_class = opcua::NodeClass::Variable,
                                 .type_definition_id = NumericNode(102)}}}});
  const auto delete_nodes = EncodeJson(ServiceRequest{
      DeleteNodesRequest{.items = {{.node_id = NumericNode(104),
                                    .delete_target_references = true}}}});
  const auto add_refs = EncodeJson(ServiceRequest{AddReferencesRequest{
      .items = {{.source_node_id = NumericNode(105),
                 .reference_type_id = NumericNode(106),
                 .forward = false,
                 .target_server_uri = "opc.tcp://server",
                 .target_node_id =
                     opcua::ExpandedNodeId{NumericNode(107), "urn:test", 2},
                 .target_node_class = opcua::NodeClass::Object}}}});
  const auto delete_refs = EncodeJson(ServiceRequest{DeleteReferencesRequest{
      .items = {{.source_node_id = NumericNode(108),
                 .reference_type_id = NumericNode(109),
                 .forward = true,
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
  HistoryReadRawResponse raw{
      .result = {
          .status = opcua::Status::FromFullCode(0x80030002u),
          .values = {opcua::DataValue{
              opcua::Variant{12.5}, opcua::Qualifier{opcua::Qualifier::MANUAL},
              ParseTime("2026-04-19 12:00:00"),
              ParseTime("2026-04-19 12:00:01")}},
          .continuation_point = {'x', 'y'}}};

  opcua::Event event;
  event.event_type_id = NumericNode(200);
  event.event_id = 201;
  event.time = ParseTime("2026-04-19 11:00:00");
  event.receive_time = ParseTime("2026-04-19 11:00:01");
  event.change_mask = opcua::Event::EVT_VAL;
  event.severity = opcua::kSeverityWarning;
  event.node_id = NumericNode(202);
  event.user_id = NumericNode(203);
  event.value = opcua::Variant{std::string{"trip"}};
  event.qualifier = opcua::Qualifier{opcua::Qualifier::OFFLINE};
  event.message = u"Alarm";
  event.acked = true;
  event.acknowledged_time = ParseTime("2026-04-19 11:05:00");
  event.acknowledged_user_id = NumericNode(204);

  HistoryReadEventsResponse events{
      .result = {.status = opcua::StatusCode::Good, .events = {event}}};

  // DataValue.qualifier is not part of the spec wire form (§5.4.2.17), so
  // round-trip per-field rather than via vector equality (which would
  // print-loop on Qualifier diffs through GTest's Variant printer).
  const auto decoded_raw = std::get<HistoryReadRawResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{raw})));
  EXPECT_EQ(decoded_raw.result.status.full_code(),
            raw.result.status.full_code());
  ASSERT_EQ(decoded_raw.result.values.size(), raw.result.values.size());
  EXPECT_TRUE(decoded_raw.result.values[0].value == raw.result.values[0].value);
  EXPECT_EQ(decoded_raw.result.values[0].status_code,
            raw.result.values[0].status_code);
  EXPECT_EQ(decoded_raw.result.values[0].source_timestamp,
            raw.result.values[0].source_timestamp);
  EXPECT_EQ(decoded_raw.result.values[0].server_timestamp,
            raw.result.values[0].server_timestamp);
  EXPECT_EQ(std::get<HistoryReadEventsResponse>(
                *DecodeServiceResponse(EncodeJson(ServiceResponse{events})))
                .result.events,
            events.result.events);
}

TEST(JsonCodecTest, RoundTripsPhase0Responses) {
  ReadResponse read{
      .status = opcua::StatusCode::Good,
      .results = {opcua::DataValue{
          opcua::Variant{opcua::LocalizedText{u"Pump"}},
          opcua::Qualifier{opcua::Qualifier::MANUAL},
          ParseTime("2026-04-19 10:10:00"), ParseTime("2026-04-19 10:10:01")}}};
  WriteResponse write{.status = opcua::StatusCode::Bad_Disconnected,
                      .results = {opcua::StatusCode::Bad_Disconnected}};
  BrowseResponse browse{
      .status = opcua::StatusCode::Good,
      .results = {{.status_code = opcua::StatusCode::Good,
                   .continuation_point = {'c', 'p'},
                   .references = {{.reference_type_id = NumericNode(301),
                                   .forward = false,
                                   .node_id = NumericNode(302)}}}}};
  BrowseNextResponse browse_next{
      .status = opcua::StatusCode::Good,
      .results = {{.status_code = opcua::StatusCode::Bad_WrongIndex},
                  {.status_code = opcua::StatusCode::Good,
                   .references = {{.reference_type_id = NumericNode(304),
                                   .forward = true,
                                   .node_id = NumericNode(305)}}}}};
  TranslateBrowsePathsResponse translate{
      .status = opcua::StatusCode::Good,
      .results = {
          {.status_code = opcua::StatusCode::Good,
           .targets = {{.target_id = opcua::ExpandedNodeId{NumericNode(303),
                                                           "urn:test", 2},
                        .remaining_path_index = 1}}}}};

  // Per OPC UA Part 6 §5.4.2.17, DataValue carries Value, Status,
  // Source/ServerTimestamp, and Source/ServerPicoseconds — no Qualifier.
  // The opcua::Qualifier is deliberately dropped on the wire, so the
  // decoded DataValue's qualifier is the default (assert per-field).
  const auto decoded_read = std::get<ReadResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{read})));
  EXPECT_EQ(decoded_read.status, read.status);
  ASSERT_EQ(decoded_read.results.size(), 1u);
  EXPECT_TRUE(decoded_read.results[0].value == read.results[0].value);
  EXPECT_EQ(decoded_read.results[0].source_timestamp,
            read.results[0].source_timestamp);
  EXPECT_EQ(decoded_read.results[0].server_timestamp,
            read.results[0].server_timestamp);
  EXPECT_EQ(decoded_read.results[0].status_code, read.results[0].status_code);

  const auto decoded_write = std::get<WriteResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{write})));
  EXPECT_EQ(decoded_write.status, write.status);
  EXPECT_EQ(decoded_write.results, write.results);

  const auto decoded_browse = std::get<BrowseResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{browse})));
  EXPECT_EQ(decoded_browse.status, browse.status);
  ASSERT_EQ(decoded_browse.results.size(), 1u);
  EXPECT_EQ(decoded_browse.results[0].status_code,
            browse.results[0].status_code);
  EXPECT_EQ(decoded_browse.results[0].continuation_point,
            browse.results[0].continuation_point);
  EXPECT_EQ(decoded_browse.results[0].references, browse.results[0].references);

  const auto decoded_browse_next = std::get<BrowseNextResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{browse_next})));
  EXPECT_EQ(decoded_browse_next.status, browse_next.status);
  EXPECT_EQ(decoded_browse_next.results, browse_next.results);

  const auto decoded_translate = std::get<TranslateBrowsePathsResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{translate})));
  EXPECT_EQ(decoded_translate.status, translate.status);
  ASSERT_EQ(decoded_translate.results.size(), 1u);
  EXPECT_EQ(decoded_translate.results[0].status_code,
            translate.results[0].status_code);
  ASSERT_EQ(decoded_translate.results[0].targets.size(), 1u);
  EXPECT_EQ(decoded_translate.results[0].targets[0].target_id,
            translate.results[0].targets[0].target_id);
  EXPECT_EQ(decoded_translate.results[0].targets[0].remaining_path_index,
            translate.results[0].targets[0].remaining_path_index);
}

TEST(JsonCodecTest, CallResponseWireShapeUsesSpecFields) {
  CallResponse response{
      .results = {
          {.status = opcua::StatusCode::Good,
           .input_argument_results = {opcua::StatusCode::Bad_WrongTypeId},
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

  const auto decoded = std::get<CallResponse>(*DecodeServiceResponse(encoded));
  ASSERT_EQ(decoded.results.size(), 1u);
  EXPECT_EQ(decoded.results[0].status.code(), opcua::StatusCode::Good);
  EXPECT_EQ(
      decoded.results[0].input_argument_results,
      (std::vector<opcua::StatusCode>{opcua::StatusCode::Bad_WrongTypeId}));
  EXPECT_EQ(decoded.results[0].output_arguments,
            (std::vector<opcua::Variant>{opcua::Variant{opcua::Int32{9}}}));
}

TEST(JsonCodecTest, RoundTripsSessionResponseMessagesAndFault) {
  ResponseMessage create{
      .request_handle = 21,
      .body = CreateSessionResponse{
          .status = opcua::StatusCode::Good,
          .session_id = NumericNode(30),
          .authentication_token = NumericNode(31, 3),
          .server_nonce = {1, 2, 3, 4},
          .revised_timeout = opcua::base::TimeDelta::FromMinutes(5),
      }};
  ResponseMessage activate{
      .request_handle = 22,
      .body = ActivateSessionResponse{
          .status = opcua::StatusCode::Bad_WrongLoginCredentials,
          .resumed = true,
      }};
  ResponseMessage close{
      .request_handle = 23,
      .body = CloseSessionResponse{.status = opcua::StatusCode::Good}};
  ResponseMessage fault{
      .request_handle = 24,
      .body = ServiceFault{.status = opcua::StatusCode::Bad_Disconnected}};

  const auto decoded_create = *DecodeResponseMessage(EncodeJson(create));
  EXPECT_EQ(decoded_create.request_handle, create.request_handle);
  const auto* create_body =
      std::get_if<CreateSessionResponse>(&decoded_create.body);
  ASSERT_NE(create_body, nullptr);
  EXPECT_EQ(create_body->status, opcua::StatusCode::Good);
  EXPECT_EQ(create_body->session_id, NumericNode(30));
  EXPECT_EQ(create_body->authentication_token, NumericNode(31, 3));
  EXPECT_EQ(create_body->server_nonce, (opcua::ByteString{1, 2, 3, 4}));
  EXPECT_EQ(create_body->revised_timeout,
            opcua::base::TimeDelta::FromMinutes(5));

  const auto decoded_activate = *DecodeResponseMessage(EncodeJson(activate));
  EXPECT_EQ(decoded_activate.request_handle, activate.request_handle);
  const auto* activate_body =
      std::get_if<ActivateSessionResponse>(&decoded_activate.body);
  ASSERT_NE(activate_body, nullptr);
  EXPECT_EQ(activate_body->status.code(),
            opcua::StatusCode::Bad_WrongLoginCredentials);
  EXPECT_TRUE(activate_body->resumed);

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
  EXPECT_EQ(fault_body->status.code(), opcua::StatusCode::Bad_Disconnected);
}

TEST(JsonCodecTest, RoundTripsServiceMessagesWithEnvelope) {
  RequestMessage request{
      .request_handle = 31,
      .body = BrowseRequest{
          .requested_max_references_per_node = 5,
          .inputs = {{.node_id = NumericNode(40),
                      .direction = opcua::BrowseDirection::Forward,
                      .reference_type_id = NumericNode(41),
                      .include_subtypes = true}}}};
  ResponseMessage response{
      .request_handle = 31,
      .body = BrowseResponse{
          .status = opcua::StatusCode::Good,
          .results = {{.status_code = opcua::StatusCode::Good,
                       .continuation_point = {'q'},
                       .references = {{.reference_type_id = NumericNode(42),
                                       .forward = true,
                                       .node_id = NumericNode(43)}}}}}};

  const auto decoded_request = *DecodeRequestMessage(EncodeJson(request));
  EXPECT_EQ(decoded_request.request_handle, request.request_handle);
  const auto* request_body = std::get_if<BrowseRequest>(&decoded_request.body);
  ASSERT_NE(request_body, nullptr);
  EXPECT_EQ(request_body->requested_max_references_per_node, 5u);
  ASSERT_EQ(request_body->inputs.size(), 1u);
  EXPECT_EQ(request_body->inputs[0].node_id, NumericNode(40));
  EXPECT_EQ(request_body->inputs[0].reference_type_id, NumericNode(41));

  const auto decoded_response = *DecodeResponseMessage(EncodeJson(response));
  EXPECT_EQ(decoded_response.request_handle, response.request_handle);
  const auto* response_body =
      std::get_if<BrowseResponse>(&decoded_response.body);
  ASSERT_NE(response_body, nullptr);
  EXPECT_EQ(response_body->status.code(), opcua::StatusCode::Good);
  ASSERT_EQ(response_body->results.size(), 1u);
  EXPECT_EQ(response_body->results[0].continuation_point,
            (opcua::ByteString{'q'}));
  ASSERT_EQ(response_body->results[0].references.size(), 1u);
  EXPECT_EQ(response_body->results[0].references[0].node_id, NumericNode(43));
}

TEST(JsonCodecTest, RoundTripsBrowseNextMessagesWithEnvelope) {
  RequestMessage request{
      .request_handle = 32,
      .body = BrowseNextRequest{.release_continuation_points = false,
                                .continuation_points = {{'a', 'b', 'c'}}}};
  ResponseMessage response{
      .request_handle = 32,
      .body = BrowseNextResponse{
          .status = opcua::StatusCode::Good,
          .results = {{.status_code = opcua::StatusCode::Good,
                       .references = {{.reference_type_id = NumericNode(52),
                                       .forward = false,
                                       .node_id = NumericNode(53)}}}}}};

  const auto decoded_request = *DecodeRequestMessage(EncodeJson(request));
  const auto* request_body =
      std::get_if<BrowseNextRequest>(&decoded_request.body);
  ASSERT_NE(request_body, nullptr);
  EXPECT_FALSE(request_body->release_continuation_points);
  EXPECT_EQ(request_body->continuation_points,
            (std::vector<opcua::ByteString>{{'a', 'b', 'c'}}));

  const auto decoded_response = *DecodeResponseMessage(EncodeJson(response));
  const auto* response_body =
      std::get_if<BrowseNextResponse>(&decoded_response.body);
  ASSERT_NE(response_body, nullptr);
  EXPECT_EQ(response_body->status.code(), opcua::StatusCode::Good);
  ASSERT_EQ(response_body->results.size(), 1u);
  EXPECT_EQ(response_body->results[0].references[0].node_id, NumericNode(53));
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
      .body = SetPublishingModeRequest{.publishing_enabled = false,
                                       .subscription_ids = {17, 18}}};
  RequestMessage delete_subscriptions{
      .request_handle = 44,
      .body = DeleteSubscriptionsRequest{.subscription_ids = {19}}};

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
      std::get<SetPublishingModeRequest>(set_mode_decoded.body);
  EXPECT_FALSE(set_mode_body.publishing_enabled);
  EXPECT_EQ(set_mode_body.subscription_ids,
            (std::vector<SubscriptionId>{17u, 18u}));

  const auto delete_decoded =
      *DecodeRequestMessage(EncodeJson(delete_subscriptions));
  EXPECT_EQ(std::get<DeleteSubscriptionsRequest>(delete_decoded.body)
                .subscription_ids,
            (std::vector<SubscriptionId>{19u}));
}

TEST(JsonCodecTest, RoundTripsMonitoredItemLifecycleMessages) {
  const boost::json::value raw_event_filter =
      boost::json::parse(R"({"Select":["Message"],"Where":{"Severity":60}})");
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
      .body = DeleteMonitoredItemsRequest{.subscription_id = 17,
                                          .monitored_item_ids = {42, 43}}};
  RequestMessage set_monitoring_mode{
      .request_handle = 54,
      .body =
          SetMonitoringModeRequest{.subscription_id = 17,
                                   .monitoring_mode = MonitoringMode::Disabled,
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
  EXPECT_EQ(std::get<DeleteMonitoredItemsRequest>(delete_decoded.body)
                .monitored_item_ids,
            (std::vector<MonitoredItemId>{42u, 43u}));

  const auto set_mode_decoded =
      *DecodeRequestMessage(EncodeJson(set_monitoring_mode));
  EXPECT_EQ(
      std::get<SetMonitoringModeRequest>(set_mode_decoded.body).monitoring_mode,
      MonitoringMode::Disabled);
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
  ResponseMessage set_publishing_mode{
      .request_handle = 63,
      .body = SetPublishingModeResponse{
          .status = opcua::StatusCode::Good,
          .results = {opcua::StatusCode::Good,
                      opcua::StatusCode::Bad_WrongSubscriptionId}}};
  ResponseMessage create_items{
      .request_handle = 64,
      .body = CreateMonitoredItemsResponse{
          .status = opcua::StatusCode::Good,
          .results = {{.status = opcua::StatusCode::Good,
                       .monitored_item_id = 42,
                       .revised_sampling_interval_ms = 250,
                       .revised_queue_size = 1},
                      {.status = opcua::StatusCode::Bad_WrongNodeId,
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
  ResponseMessage delete_items{.request_handle = 66,
                               .body = DeleteMonitoredItemsResponse{
                                   .status = opcua::StatusCode::Good,
                                   .results = {opcua::StatusCode::Good}}};
  ResponseMessage set_monitoring_mode{
      .request_handle = 67,
      .body = SetMonitoringModeResponse{
          .status = opcua::StatusCode::Good,
          .results = {opcua::StatusCode::Good,
                      opcua::StatusCode::Bad_WrongSubscriptionId}}};

  EXPECT_EQ(std::get<CreateSubscriptionResponse>(
                (*DecodeResponseMessage(EncodeJson(create_subscription))).body)
                .subscription_id,
            17u);
  EXPECT_EQ(std::get<ModifySubscriptionResponse>(
                (*DecodeResponseMessage(EncodeJson(modify_subscription))).body)
                .revised_max_keep_alive_count,
            5u);
  EXPECT_EQ(std::get<SetPublishingModeResponse>(
                (*DecodeResponseMessage(EncodeJson(set_publishing_mode))).body)
                .results,
            (std::vector<opcua::StatusCode>{
                opcua::StatusCode::Good,
                opcua::StatusCode::Bad_WrongSubscriptionId}));

  const auto encoded_create_items = EncodeJson(create_items);
  const auto& encoded_create_items_body =
      encoded_create_items.at("body").as_object();
  const auto& encoded_create_items_results =
      encoded_create_items_body.at("Results").as_array();
  ASSERT_EQ(encoded_create_items_results.size(), 2u);
  const auto& first_create_result = encoded_create_items_results[0].as_object();
  EXPECT_TRUE(first_create_result.if_contains("StatusCode"));
  EXPECT_FALSE(first_create_result.if_contains("Status"));

  const auto decoded_create_items =
      *DecodeResponseMessage(EncodeJson(create_items));
  const auto& create_items_body =
      std::get<CreateMonitoredItemsResponse>(decoded_create_items.body);
  ASSERT_EQ(create_items_body.results.size(), 2u);
  EXPECT_EQ(create_items_body.results[0].status.code(),
            opcua::StatusCode::Good);
  EXPECT_EQ(create_items_body.results[1].status.code(),
            opcua::StatusCode::Bad_WrongNodeId);
  ASSERT_TRUE(create_items_body.results[1].filter_result.has_value());
  EXPECT_EQ(*create_items_body.results[1].filter_result, filter_result);

  EXPECT_EQ(std::get<ModifyMonitoredItemsResponse>(
                (*DecodeResponseMessage(EncodeJson(modify_items))).body)
                .results[0]
                .revised_queue_size,
            8u);
  EXPECT_EQ(std::get<DeleteMonitoredItemsResponse>(
                (*DecodeResponseMessage(EncodeJson(delete_items))).body)
                .results,
            (std::vector<opcua::StatusCode>{opcua::StatusCode::Good}));
  EXPECT_EQ(std::get<SetMonitoringModeResponse>(
                (*DecodeResponseMessage(EncodeJson(set_monitoring_mode))).body)
                .results,
            (std::vector<opcua::StatusCode>{
                opcua::StatusCode::Good,
                opcua::StatusCode::Bad_WrongSubscriptionId}));
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
      .body = TransferSubscriptionsRequest{.subscription_ids = {17, 18},
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
      std::get<TransferSubscriptionsRequest>(transfer_decoded.body);
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
  ResponseMessage transfer{
      .request_handle = 83,
      .body = TransferSubscriptionsResponse{
          .status = opcua::StatusCode::Good,
          .results = {opcua::StatusCode::Good,
                      opcua::StatusCode::Bad_WrongSubscriptionId}}};

  std::optional<boost::json::value> publish_json;
  ASSERT_NO_THROW(publish_json.emplace(EncodeJson(publish)));
  const auto& notification_json = publish_json->as_object()
                                      .at("body")
                                      .as_object()
                                      .at("NotificationMessage")
                                      .as_object();
  EXPECT_EQ(notification_json.at("PublishTime").as_string(),
            "2026-04-19T00:00:05Z");
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
  EXPECT_EQ(
      std::get<TransferSubscriptionsResponse>(decoded_transfer->body).results,
      (std::vector<opcua::StatusCode>{
          opcua::StatusCode::Good,
          opcua::StatusCode::Bad_WrongSubscriptionId}));
}

TEST(JsonCodecTest, RoundTripsCallAndMutationResponses) {
  CallResponse call{
      .results = {{.status = opcua::StatusCode::Good},
                  {.status = opcua::StatusCode::Bad_WrongCallArguments}}};
  AddNodesResponse add_nodes{
      .status = opcua::StatusCode::Good,
      .results = {{.status_code = opcua::StatusCode::Good,
                   .added_node_id = NumericNode(300)}}};
  DeleteNodesResponse delete_nodes{
      .status = opcua::StatusCode::Bad_Disconnected,
      .results = {opcua::StatusCode::Bad_Disconnected}};
  AddReferencesResponse add_refs{
      .status = opcua::StatusCode::Good,
      .results = {opcua::StatusCode::Good,
                  opcua::StatusCode::Bad_WrongTargetId}};
  DeleteReferencesResponse delete_refs{.status = opcua::StatusCode::Good,
                                       .results = {opcua::StatusCode::Good}};

  const auto decoded_call = std::get<CallResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{call})));
  ASSERT_EQ(decoded_call.results.size(), call.results.size());
  EXPECT_EQ(decoded_call.results[0].status.code(),
            call.results[0].status.code());
  EXPECT_EQ(decoded_call.results[1].status.code(),
            call.results[1].status.code());
  const auto decoded_add_nodes = std::get<AddNodesResponse>(
      *DecodeServiceResponse(EncodeJson(ServiceResponse{add_nodes})));
  EXPECT_EQ(decoded_add_nodes.status, add_nodes.status);
  ASSERT_EQ(decoded_add_nodes.results.size(), 1u);
  EXPECT_EQ(decoded_add_nodes.results[0].status_code,
            add_nodes.results[0].status_code);
  EXPECT_EQ(decoded_add_nodes.results[0].added_node_id,
            add_nodes.results[0].added_node_id);
  EXPECT_EQ(std::get<DeleteNodesResponse>(*DecodeServiceResponse(EncodeJson(
                                              ServiceResponse{delete_nodes})))
                .results,
            delete_nodes.results);
  EXPECT_EQ(std::get<AddReferencesResponse>(
                *DecodeServiceResponse(EncodeJson(ServiceResponse{add_refs})))
                .results,
            add_refs.results);
  EXPECT_EQ(
      std::get<DeleteReferencesResponse>(
          *DecodeServiceResponse(EncodeJson(ServiceResponse{delete_refs})))
          .results,
      delete_refs.results);
}

TEST(JsonCodecTest, RejectsUnknownService) {
  boost::json::value json = boost::json::object{
      {"service", "Unknown"}, {"body", boost::json::object{}}};
  EXPECT_EQ(DecodeServiceRequest(json).status().code(),
            opcua::StatusCode::Bad_CantParseString);
  EXPECT_EQ(DecodeServiceResponse(json).status().code(),
            opcua::StatusCode::Bad_CantParseString);
}

}  // namespace
}  // namespace opcua::ws
