#include "opcua/ua/ua_binary_codec.h"

#include "opcua/transport/binary/service_codec.h"

#include <gtest/gtest.h>

namespace opcua::ua {
namespace {

using binary::Decoder;
using binary::Encoder;

// Decodes a frame produced by the hand-written service codec with the
// generated one.
//
// This is the strongest check available for the generated codec: the
// hand-written encoder is the implementation that was verified live against
// open62541 and UAExpert in the 2026-07-18 conformance pass, so agreeing with
// it byte-for-byte means agreeing with the spec. `consumed()` is what makes it
// bite — it fails unless the generated struct's field list and order match what
// the hand-written encoder wrote, field for field, with nothing left over.
template <class T>
T DecodeHandWrittenRequest(const RequestBody& request,
                           std::uint32_t expected_encoding_id) {
  const binary::ServiceRequestHeader header{
      .authentication_token = NodeId{7, 1}, .request_handle = 42};
  const std::optional<std::vector<char>> bytes =
      binary::EncodeServiceRequest(header, request);
  EXPECT_TRUE(bytes.has_value());

  Decoder decoder{*bytes};
  const auto message = binary::ReadMessage(decoder);
  EXPECT_TRUE(message.has_value());
  EXPECT_EQ(message->first, expected_encoding_id);

  Decoder body{message->second};
  T decoded;
  EXPECT_TRUE(Decode(body, decoded));
  EXPECT_TRUE(body.consumed())
      << "generated decoder left " << body.remaining().size()
      << " bytes unread — its field layout disagrees with the hand-written "
         "encoder";
  return decoded;
}

TEST(UaBinaryCodecTest, DecodesHandWrittenReadRequest) {
  opcua::ReadRequest request;
  request.timestamps_to_return = 2;
  request.inputs.push_back(
      {.node_id = NodeId{2253, 0}, .attribute_id = opcua::AttributeId::Value});
  request.inputs.push_back({.node_id = NodeId{String{"Some.Node"}, 3},
                            .attribute_id = opcua::AttributeId::DisplayName});

  const ReadRequest decoded = DecodeHandWrittenRequest<ReadRequest>(
      request, binary_encoding_id::kReadRequest);

  EXPECT_EQ(decoded.request_header.request_handle, 42u);
  EXPECT_EQ(decoded.request_header.authentication_token, (NodeId{7, 1}));
  EXPECT_EQ(decoded.timestamps_to_return, TimestampsToReturn::Both);
  ASSERT_EQ(decoded.nodes_to_read.size(), 2u);
  EXPECT_EQ(decoded.nodes_to_read[0].node_id, (NodeId{2253, 0}));
  EXPECT_EQ(decoded.nodes_to_read[0].attribute_id,
            static_cast<UInt32>(opcua::AttributeId::Value));
  EXPECT_EQ(decoded.nodes_to_read[1].node_id, (NodeId{String{"Some.Node"}, 3}));
  // Fields the hand-written ReadValueId dropped on the floor are visible now.
  EXPECT_TRUE(decoded.nodes_to_read[0].index_range.empty());
  EXPECT_TRUE(decoded.nodes_to_read[0].data_encoding.empty());
}

TEST(UaBinaryCodecTest, DecodesHandWrittenBrowseRequest) {
  opcua::BrowseRequest request;
  request.requested_max_references_per_node = 17;
  request.inputs.push_back({.node_id = NodeId{85, 0},
                            .direction = opcua::BrowseDirection::Forward,
                            .reference_type_id = NodeId{33, 0},
                            .include_subtypes = true,
                            .node_class_mask = 0xff,
                            .result_mask = 0x3f});

  const BrowseRequest decoded = DecodeHandWrittenRequest<BrowseRequest>(
      request, binary_encoding_id::kBrowseRequest);

  EXPECT_EQ(decoded.requested_max_references_per_node, 17u);
  ASSERT_EQ(decoded.nodes_to_browse.size(), 1u);
  EXPECT_EQ(decoded.nodes_to_browse[0].node_id, (NodeId{85, 0}));
  EXPECT_EQ(decoded.nodes_to_browse[0].browse_direction,
            ua::BrowseDirection::Forward);
  EXPECT_EQ(decoded.nodes_to_browse[0].reference_type_id, (NodeId{33, 0}));
  EXPECT_TRUE(decoded.nodes_to_browse[0].include_subtypes);
  EXPECT_EQ(decoded.nodes_to_browse[0].node_class_mask, 0xffu);
  EXPECT_EQ(decoded.nodes_to_browse[0].result_mask, 0x3fu);
}

TEST(UaBinaryCodecTest, DecodesHandWrittenCreateSubscriptionRequest) {
  opcua::CreateSubscriptionRequest request;
  request.parameters = {.publishing_interval_ms = 250.0,
                        .lifetime_count = 60,
                        .max_keep_alive_count = 20,
                        .max_notifications_per_publish = 100,
                        .publishing_enabled = true,
                        .priority = 5};

  const CreateSubscriptionRequest decoded =
      DecodeHandWrittenRequest<CreateSubscriptionRequest>(
          request, binary_encoding_id::kCreateSubscriptionRequest);

  EXPECT_DOUBLE_EQ(decoded.requested_publishing_interval, 250.0);
  EXPECT_EQ(decoded.requested_lifetime_count, 60u);
  EXPECT_EQ(decoded.requested_max_keep_alive_count, 20u);
  EXPECT_EQ(decoded.max_notifications_per_publish, 100u);
  EXPECT_TRUE(decoded.publishing_enabled);
  EXPECT_EQ(decoded.priority, 5);
}

// The generated codec on its own terms: every field survives a round trip,
// including the ones the hand-written types never modelled.
TEST(UaBinaryCodecTest, RoundTripsEveryFieldOfAService) {
  ReadRequest request;
  request.request_header.authentication_token = NodeId{String{"token"}, 1};
  request.request_header.timestamp = DateTime::FromInternalValue(998877);
  request.request_header.request_handle = 9;
  request.request_header.return_diagnostics = 0x3ff;
  request.request_header.audit_entry_id = "audit";
  request.request_header.timeout_hint = 5000;
  request.max_age = 1500.5;
  request.timestamps_to_return = TimestampsToReturn::Source;
  request.nodes_to_read.push_back(
      {.node_id = NodeId{99, 4},
       .attribute_id = 13,
       .index_range = "1:3",
       .data_encoding = QualifiedName{"Binary", 0}});

  ByteString bytes;
  Encoder encoder{bytes};
  Encode(encoder, request);

  Decoder decoder{bytes};
  ReadRequest decoded;
  ASSERT_TRUE(Decode(decoder, decoded));
  EXPECT_TRUE(decoder.consumed());

  EXPECT_EQ(decoded.request_header.authentication_token,
            (NodeId{String{"token"}, 1}));
  EXPECT_EQ(decoded.request_header.timestamp,
            DateTime::FromInternalValue(998877));
  EXPECT_EQ(decoded.request_header.request_handle, 9u);
  EXPECT_EQ(decoded.request_header.return_diagnostics, 0x3ffu);
  EXPECT_EQ(decoded.request_header.audit_entry_id, "audit");
  EXPECT_EQ(decoded.request_header.timeout_hint, 5000u);
  EXPECT_DOUBLE_EQ(decoded.max_age, 1500.5);
  EXPECT_EQ(decoded.timestamps_to_return, TimestampsToReturn::Source);
  ASSERT_EQ(decoded.nodes_to_read.size(), 1u);
  EXPECT_EQ(decoded.nodes_to_read[0].index_range, "1:3");
  EXPECT_EQ(decoded.nodes_to_read[0].data_encoding,
            (QualifiedName{"Binary", 0}));
}

// Enumerations travel as their declared width, not as whatever the C++
// enumerator happens to be.
TEST(UaBinaryCodecTest, EncodesEnumerationsAtTheirDeclaredWidth) {
  ByteString bytes;
  Encoder encoder{bytes};
  Encode(encoder, TimestampsToReturn::Server);
  ASSERT_EQ(bytes.size(), 4u);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 1u);

  Decoder decoder{bytes};
  TimestampsToReturn decoded{};
  ASSERT_TRUE(Decode(decoder, decoded));
  EXPECT_EQ(decoded, TimestampsToReturn::Server);
  EXPECT_TRUE(decoder.consumed());
}

TEST(UaBinaryCodecTest, RoundTripsStructuredValuesThroughAnExtensionObject) {
  ElementOperand operand;
  operand.index = 3;

  const ExtensionObject wrapped = ToExtensionObject(operand);
  EXPECT_EQ(wrapped.data_type_id().node_id(),
            NodeId{BinaryEncodingId<ElementOperand>::value});

  ElementOperand decoded;
  ASSERT_TRUE(FromExtensionObject(wrapped, decoded));
  EXPECT_EQ(decoded.index, 3u);

  // A body carrying some other type is rejected rather than misread.
  LiteralOperand other;
  EXPECT_FALSE(FromExtensionObject(ToExtensionObject(other), decoded));
}

// An ExtensionObject whose type id this stack does not know keeps its body and
// round-trips unchanged, so a message carrying one is still forwardable.
TEST(UaBinaryCodecTest, PreservesUnknownExtensionObjectBodies) {
  const ExtensionObject unknown{ExpandedNodeId{NodeId{999999, 0}},
                                ByteString{'\x01', '\x02', '\x03'}};

  ByteString bytes;
  Encoder encoder{bytes};
  encoder.Encode(unknown);

  Decoder decoder{bytes};
  ExtensionObject decoded;
  ASSERT_TRUE(decoder.Decode(decoded));
  EXPECT_TRUE(decoder.consumed());
  EXPECT_EQ(decoded.data_type_id().node_id(), (NodeId{999999, 0}));
  ASSERT_NE(decoded.binary_body(), nullptr);
  EXPECT_EQ(*decoded.binary_body(), (ByteString{'\x01', '\x02', '\x03'}));
  // ExtensionObject equality used to be hardcoded to false, which made every
  // containing type unequal to itself.
  EXPECT_EQ(decoded, unknown);
  EXPECT_NE(decoded, ExtensionObject{});
}

// Arrays are an Int32 count followed by the elements; a negative count means a
// null array and decodes as empty.
TEST(UaBinaryCodecTest, TreatsNegativeArrayCountAsEmpty) {
  ByteString bytes;
  Encoder encoder{bytes};
  Encode(encoder, RequestHeader{});
  encoder.Encode(Double{0});
  encoder.Encode(static_cast<Int32>(TimestampsToReturn::Neither));
  encoder.Encode(Int32{-1});

  Decoder decoder{bytes};
  ReadRequest decoded;
  ASSERT_TRUE(Decode(decoder, decoded));
  EXPECT_TRUE(decoder.consumed());
  EXPECT_TRUE(decoded.nodes_to_read.empty());
}

// A hostile element count must not turn into a huge reservation.
TEST(UaBinaryCodecTest, RejectsArrayCountLargerThanTheRemainingBytes) {
  ByteString bytes;
  Encoder encoder{bytes};
  Encode(encoder, RequestHeader{});
  encoder.Encode(Double{0});
  encoder.Encode(static_cast<Int32>(TimestampsToReturn::Neither));
  encoder.Encode(Int32{0x7fffffff});

  Decoder decoder{bytes};
  ReadRequest decoded;
  EXPECT_FALSE(Decode(decoder, decoded));
}

}  // namespace
}  // namespace opcua::ua
