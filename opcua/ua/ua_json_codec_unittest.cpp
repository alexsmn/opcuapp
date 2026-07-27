#include "opcua/ua/ua_json_codec.h"

#include "opcua/ua/ua_binary_codec.h"

#include "opcua/base/time_utils.h"

#include <boost/json.hpp>

#include <gtest/gtest.h>

namespace opcua::ua {
namespace {

using boost::json::object;
using boost::json::value;

// Round-trips a value through the generated JSON codec.
template <class T>
T RoundTrip(const T& original) {
  const value json = EncodeJson(original);
  T decoded;
  DecodeJson(json, decoded);
  return decoded;
}

TEST(UaJsonCodecTest, RoundTripsAReadRequestWithEveryFieldSet) {
  ReadRequest request;
  request.request_header.authentication_token = NodeId{String{"token"}, 1};
  request.request_header.request_handle = 77;
  request.request_header.timeout_hint = 3000;
  request.max_age = 250.5;
  request.timestamps_to_return = TimestampsToReturn::Source;
  request.nodes_to_read.push_back(
      {.node_id = NodeId{2253, 0},
       .attribute_id = 13,
       .index_range = "0:4",
       .data_encoding = QualifiedName{"Binary", 0}});

  const ReadRequest decoded = RoundTrip(request);
  EXPECT_EQ(decoded.request_header.authentication_token,
            (NodeId{String{"token"}, 1}));
  EXPECT_EQ(decoded.request_header.request_handle, 77u);
  EXPECT_EQ(decoded.request_header.timeout_hint, 3000u);
  EXPECT_DOUBLE_EQ(decoded.max_age, 250.5);
  EXPECT_EQ(decoded.timestamps_to_return, TimestampsToReturn::Source);
  ASSERT_EQ(decoded.nodes_to_read.size(), 1u);
  EXPECT_EQ(decoded.nodes_to_read[0].node_id, (NodeId{2253, 0}));
  EXPECT_EQ(decoded.nodes_to_read[0].index_range, "0:4");
  EXPECT_EQ(decoded.nodes_to_read[0].data_encoding,
            (QualifiedName{"Binary", 0}));
}

// The compact form uses the spec's field names and omits defaults. Pinning the
// JSON shape here is what keeps the codec conformant — it is exactly what a
// third-party JSON client reads.
TEST(UaJsonCodecTest, UsesSpecFieldNamesAndOmitsDefaults) {
  ReadRequest request;
  request.timestamps_to_return = TimestampsToReturn::Both;
  request.nodes_to_read.push_back({.node_id = NodeId{85, 0}});

  const object json = EncodeJson(request).as_object();
  EXPECT_TRUE(json.contains("RequestHeader"));
  EXPECT_TRUE(json.contains("TimestampsToReturn"));
  EXPECT_TRUE(json.contains("NodesToRead"));
  // MaxAge holds its default (0) and is omitted.
  EXPECT_FALSE(json.contains("MaxAge"));

  const object& read_value = json.at("NodesToRead").as_array()[0].as_object();
  // NodeId is the text form, not an object.
  EXPECT_EQ(read_value.at("NodeId").as_string(), "i=85");
  // AttributeId 0 is a default and omitted; IndexRange/DataEncoding too.
  EXPECT_FALSE(read_value.contains("AttributeId"));
  EXPECT_FALSE(read_value.contains("IndexRange"));
  EXPECT_FALSE(read_value.contains("DataEncoding"));
}

// The 1.05 DataValue inlines the Variant: UaType/Value are the DataValue's own
// members, not a nested Variant object. (The pre-generation websocket codec
// nested a `{Type, Body}` Value instead.)
TEST(UaJsonCodecTest, InlinesTheVariantInsideADataValue) {
  // A whole-second timestamp: opcuapp's DateTime text parser does not read the
  // fractional-second form the encoder can emit (a pre-existing limitation of
  // the hand-written DateTime codec, independent of this codec).
  DateTime source_timestamp;
  ASSERT_TRUE(Deserialize("2026-04-19 10:00:00", source_timestamp));
  DataValue data_value;
  data_value.value = Variant{Int32{-9}};
  data_value.status_code = StatusCode::Uncertain;
  data_value.source_timestamp = source_timestamp;

  const object json = json::Encode(data_value).as_object();
  EXPECT_EQ(json.at("UaType").to_number<int>(), Variant::INT32);
  EXPECT_EQ(json.at("Value").as_int64(), -9);
  EXPECT_TRUE(json.contains("StatusCode"));
  EXPECT_TRUE(json.contains("SourceTimestamp"));
  // A nested Variant object would be wrong.
  EXPECT_FALSE(json.contains("Type"));
  EXPECT_FALSE(json.contains("Body"));

  DataValue decoded;
  json::Decode(json, decoded);
  EXPECT_EQ(decoded.value.get<Int32>(), -9);
  EXPECT_EQ(decoded.status_code, StatusCode::Uncertain);
  EXPECT_EQ(decoded.source_timestamp, data_value.source_timestamp);
}

// StatusCode is an object with a numeric Code, not a bare number (Part 6
// §5.4.2.12). A decoder still accepts the bare number for tolerance.
TEST(UaJsonCodecTest, EncodesStatusCodeAsAnObject) {
  const value json = json::Encode(Status{StatusCode::Bad_NodeIdUnknown});
  ASSERT_TRUE(json.is_object());
  EXPECT_EQ(json.as_object().at("Code").as_uint64(),
            Status{StatusCode::Bad_NodeIdUnknown}.full_code());

  Status decoded_object{StatusCode::Good};
  json::Decode(json, decoded_object);
  EXPECT_EQ(decoded_object.code(), StatusCode::Bad_NodeIdUnknown);

  // Good is the default and encodes as an empty object.
  EXPECT_TRUE(json::Encode(Status{StatusCode::Good}).as_object().empty());

  // Tolerant decode of the legacy bare-number form.
  Status decoded_number{StatusCode::Good};
  json::Decode(value(Status{StatusCode::Bad_NodeIdUnknown}.full_code()),
               decoded_number);
  EXPECT_EQ(decoded_number.code(), StatusCode::Bad_NodeIdUnknown);
}

// ByteString is base64 text (Part 6 §5.4.2.7), not a JSON array of bytes as the
// pre-generation websocket codec emitted.
TEST(UaJsonCodecTest, EncodesByteStringAsBase64) {
  const ByteString bytes{'\x00', '\x01', '\xfe', '\xff'};
  const value json = json::Encode(bytes);
  ASSERT_TRUE(json.is_string());
  EXPECT_EQ(json.as_string(), "AAH+/w==");

  ByteString decoded;
  json::Decode(json, decoded);
  EXPECT_EQ(decoded, bytes);
}

// Int64/UInt64 travel as strings so JSON's double cannot lose the low bits.
TEST(UaJsonCodecTest, EncodesSixtyFourBitIntegersAsStrings) {
  constexpr Int64 kLarge = 9223372036854775807LL;
  const value json = json::Encode(kLarge);
  ASSERT_TRUE(json.is_string());
  EXPECT_EQ(json.as_string(), "9223372036854775807");

  Int64 decoded = 0;
  json::Decode(json, decoded);
  EXPECT_EQ(decoded, kLarge);
}

TEST(UaJsonCodecTest, RoundTripsAVariantThroughAnInlinedField) {
  WriteRequest request;
  request.nodes_to_write.push_back(
      {.node_id = NodeId{1234, 2}, .attribute_id = 13, .value = DataValue{}});
  request.nodes_to_write[0].value.value = Variant{std::vector<Int32>{1, 2, 3}};

  const WriteRequest decoded = RoundTrip(request);
  ASSERT_EQ(decoded.nodes_to_write.size(), 1u);
  EXPECT_EQ(decoded.nodes_to_write[0].value.value.get<std::vector<Int32>>(),
            (std::vector<Int32>{1, 2, 3}));
}

// A structured value inside an ExtensionObject survives the JSON round trip
// through the binary body (UaEncoding 1).
TEST(UaJsonCodecTest, RoundTripsAStructuredExtensionObject) {
  ElementOperand operand;
  operand.index = 7;
  const ExtensionObject wrapped = ToExtensionObject(operand);

  const value json = json::Encode(wrapped);
  ASSERT_TRUE(json.is_object());
  EXPECT_EQ(json.as_object().at("UaEncoding").as_int64(), 1);

  ExtensionObject decoded;
  json::Decode(json, decoded);
  ElementOperand decoded_operand;
  ASSERT_TRUE(FromExtensionObject(decoded, decoded_operand));
  EXPECT_EQ(decoded_operand.index, 7u);
}

// The JSON flavour: the body is the type's own JSON encoding, keyed by the
// DefaultJson id, with no UaEncoding marker (Part 6 §5.4.2.16).
TEST(UaJsonCodecTest, RoundTripsAJsonBodiedExtensionObject) {
  ElementOperand operand;
  operand.index = 7;
  const ExtensionObject wrapped = ToJsonExtensionObject(operand);

  const value json = json::Encode(wrapped);
  ASSERT_TRUE(json.is_object());
  EXPECT_FALSE(json.as_object().contains("UaEncoding"));
  EXPECT_TRUE(json.as_object().at("UaBody").is_object());

  ExtensionObject decoded;
  json::Decode(json, decoded);
  ElementOperand decoded_operand;
  ASSERT_TRUE(FromJsonExtensionObject(decoded, decoded_operand));
  EXPECT_EQ(decoded_operand.index, 7u);
}

// The two flavours are not interchangeable: each helper rejects the other's
// body rather than mis-decoding it. This is the bug that kept HistoryRead's
// details from crossing the UA-JSON transport.
TEST(UaJsonCodecTest, BinaryAndJsonExtensionObjectHelpersDoNotCross) {
  ElementOperand operand;
  operand.index = 7;

  ElementOperand out;
  EXPECT_FALSE(FromJsonExtensionObject(ToExtensionObject(operand), out));
  EXPECT_FALSE(FromExtensionObject(ToJsonExtensionObject(operand), out));
}

// A peer that names a JSON body by its DefaultBinary id is still unambiguous
// about which structure it means, so the JSON helper accepts it.
TEST(UaJsonCodecTest, JsonExtensionObjectAcceptsTheBinaryEncodingId) {
  ElementOperand operand;
  operand.index = 7;
  const ExtensionObject wrapped{
      ExpandedNodeId{NodeId{ElementOperand::kBinaryEncodingId}},
      EncodeJson(operand)};

  ElementOperand decoded;
  ASSERT_TRUE(FromJsonExtensionObject(wrapped, decoded));
  EXPECT_EQ(decoded.index, 7u);
}

// A different type's id, and a body that does not decode, are both reported as
// a mismatch rather than throwing.
TEST(UaJsonCodecTest, JsonExtensionObjectRejectsMismatches) {
  ElementOperand operand;
  operand.index = 7;

  LiteralOperand wrong_type;
  EXPECT_FALSE(FromJsonExtensionObject(ToJsonExtensionObject(operand),
                                       wrong_type));

  const ExtensionObject malformed{
      ExpandedNodeId{NodeId{ElementOperand::kJsonEncodingId}},
      value{"not an object"}};
  ElementOperand decoded;
  EXPECT_FALSE(FromJsonExtensionObject(malformed, decoded));

  // An empty ExtensionObject carries no body at all.
  ElementOperand from_empty;
  EXPECT_FALSE(FromJsonExtensionObject(ExtensionObject{}, from_empty));
}

// The HistoryRead details that the websocket transport has to carry. This is
// the case the helpers exist for: over UA-JSON these arrive JSON-bodied, and
// ua::FromExtensionObject cannot read them.
TEST(UaJsonCodecTest, RoundTripsHistoryReadDetailsAsJsonExtensionObjects) {
  ReadRawModifiedDetails raw;
  raw.num_values_per_node = 123;
  raw.return_bounds = false;
  const ExtensionObject raw_wrapped = ToJsonExtensionObject(raw);
  EXPECT_EQ(raw_wrapped.data_type_id().node_id().numeric_id(),
            ReadRawModifiedDetails::kJsonEncodingId);

  ReadRawModifiedDetails decoded_raw;
  ASSERT_TRUE(FromJsonExtensionObject(raw_wrapped, decoded_raw));
  EXPECT_EQ(decoded_raw.num_values_per_node, 123u);

  // ReadEventDetails must not be readable out of a ReadRawModifiedDetails
  // body — the details type is what selects raw vs events.
  ReadEventDetails decoded_events;
  EXPECT_FALSE(FromJsonExtensionObject(raw_wrapped, decoded_events));

  ReadEventDetails events;
  events.num_values_per_node = 7;
  ReadEventDetails round_tripped;
  ASSERT_TRUE(
      FromJsonExtensionObject(ToJsonExtensionObject(events), round_tripped));
  EXPECT_EQ(round_tripped.num_values_per_node, 7u);
}

// Enumerations are JSON integers of their declared value.
TEST(UaJsonCodecTest, EncodesEnumerationsAsIntegers) {
  const value json = EncodeJson(BrowseDirection::Inverse);
  ASSERT_TRUE(json.is_int64() || json.is_uint64());
  EXPECT_EQ(json.to_number<int>(), static_cast<int>(BrowseDirection::Inverse));

  BrowseDirection decoded{};
  DecodeJson(json, decoded);
  EXPECT_EQ(decoded, BrowseDirection::Inverse);
}

// A LocalizedText round-trips its locale and text, each omitted when empty.
TEST(UaJsonCodecTest, RoundTripsLocalizedText) {
  const LocalizedText text{"ru", u"Значение"};
  const value json = json::Encode(text);
  EXPECT_EQ(json.as_object().at("Locale").as_string(), "ru");

  LocalizedText decoded;
  json::Decode(json, decoded);
  EXPECT_EQ(decoded, text);

  // Empty encodes as an empty object.
  EXPECT_TRUE(json::Encode(LocalizedText{}).as_object().empty());
}

}  // namespace
}  // namespace opcua::ua
