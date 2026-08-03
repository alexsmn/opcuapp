#include "opcua/transport/binary/codec_utils.h"

#include <algorithm>
#include <limits>

#include <gtest/gtest.h>

namespace opcua::binary {
namespace {

TEST(CodecUtilsTest, RoundTripsPrimitiveValues) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::uint8_t{0xab});
  encoder.Encode(std::uint16_t{0x1234});
  encoder.Encode(std::uint32_t{0x89abcdef});
  encoder.Encode(true);
  encoder.Encode(std::int32_t{-1234567});
  encoder.Encode(std::int64_t{-9876543210LL});
  encoder.Encode(12.5);

  Decoder decoder{bytes};
  std::uint8_t u8 = 0;
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  bool boolean = false;
  std::int32_t i32 = 0;
  std::int64_t i64 = 0;
  double dbl = 0;
  EXPECT_TRUE(decoder.Decode(u8));
  EXPECT_TRUE(decoder.Decode(u16));
  EXPECT_TRUE(decoder.Decode(u32));
  EXPECT_TRUE(decoder.Decode(boolean));
  EXPECT_TRUE(decoder.Decode(i32));
  EXPECT_TRUE(decoder.Decode(i64));
  EXPECT_TRUE(decoder.Decode(dbl));

  EXPECT_EQ(u8, 0xab);
  EXPECT_EQ(u16, 0x1234);
  EXPECT_EQ(u32, 0x89abcdefu);
  EXPECT_TRUE(boolean);
  EXPECT_EQ(i32, -1234567);
  EXPECT_EQ(i64, -9876543210LL);
  EXPECT_DOUBLE_EQ(dbl, 12.5);
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, HandlesStringsAndByteStrings) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::string_view{"opc.tcp://localhost:4840"});
  encoder.Encode(opcua::QualifiedName{"BrowseName", 2});
  encoder.Encode(opcua::ToLocalizedText(std::u16string_view{u"DisplayName"}));
  encoder.Encode(opcua::ByteString{'a', 'b', 'c'});
  encoder.Encode(std::int32_t{-1});
  encoder.Encode(std::int32_t{-1});

  Decoder decoder{bytes};
  std::string string_value;
  opcua::QualifiedName qualified_name;
  opcua::LocalizedText localized_text;
  opcua::ByteString byte_string;
  std::string null_string = "sentinel";
  opcua::ByteString null_bytes = {'x'};
  EXPECT_TRUE(decoder.Decode(string_value));
  EXPECT_TRUE(decoder.Decode(qualified_name));
  EXPECT_TRUE(decoder.Decode(localized_text));
  EXPECT_TRUE(decoder.Decode(byte_string));
  EXPECT_TRUE(decoder.Decode(null_string));
  EXPECT_TRUE(decoder.Decode(null_bytes));

  EXPECT_EQ(string_value, "opc.tcp://localhost:4840");
  EXPECT_EQ(qualified_name, (opcua::QualifiedName{"BrowseName", 2}));
  EXPECT_EQ(localized_text,
            opcua::ToLocalizedText(std::u16string_view{u"DisplayName"}));
  EXPECT_EQ(byte_string, (opcua::ByteString{'a', 'b', 'c'}));
  EXPECT_TRUE(null_string.empty());
  EXPECT_TRUE(null_bytes.empty());
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, PreservesDateTimeBinaryRepresentation) {
  constexpr std::int64_t kRawValue =
      opcua::DateTime::kTimeTToDateTimeTicksOffset + 7;
  const auto date_time = opcua::DateTime::FromInternalValue(kRawValue);

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(date_time);
  encoder.Encode(opcua::DateTime::Max());

  Decoder raw_decoder{bytes};
  std::int64_t raw_value = 0;
  std::int64_t raw_max = 0;
  ASSERT_TRUE(raw_decoder.Decode(raw_value));
  ASSERT_TRUE(raw_decoder.Decode(raw_max));
  EXPECT_EQ(raw_value, kRawValue);
  EXPECT_EQ(raw_max, std::numeric_limits<std::int64_t>::max());

  Decoder date_time_decoder{bytes};
  opcua::DateTime decoded;
  opcua::DateTime decoded_max;
  ASSERT_TRUE(date_time_decoder.Decode(decoded));
  ASSERT_TRUE(date_time_decoder.Decode(decoded_max));
  EXPECT_EQ(decoded.ToInternalValue(), kRawValue);
  EXPECT_TRUE(decoded_max.is_max());
}

// OPC UA Part 6 §5.2.2.14: the Locale field (mask bit 0x01) must survive the
// binary round trip alongside Text (mask bit 0x02).
TEST(CodecUtilsTest, RoundTripsLocalizedTextLocale) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::LocalizedText{"ru", u"Значение"});
  encoder.Encode(opcua::LocalizedText{"en", {}});
  encoder.Encode(opcua::LocalizedText{u"text only"});

  // First byte of the first value: both Locale and Text present.
  ASSERT_GE(bytes.size(), 1u);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0x03u);

  Decoder decoder{bytes};
  opcua::LocalizedText localized;
  opcua::LocalizedText locale_only;
  opcua::LocalizedText text_only;
  ASSERT_TRUE(decoder.Decode(localized));
  ASSERT_TRUE(decoder.Decode(locale_only));
  ASSERT_TRUE(decoder.Decode(text_only));
  EXPECT_EQ(localized, (opcua::LocalizedText{"ru", u"Значение"}));
  EXPECT_EQ(locale_only, (opcua::LocalizedText{"en", {}}));
  EXPECT_EQ(text_only, opcua::LocalizedText{u"text only"});
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, EncodesEmptyLocalizedTextAsMaskOnly) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::LocalizedText{});

  ASSERT_EQ(bytes.size(), 1u);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0u);

  Decoder decoder{bytes};
  opcua::LocalizedText decoded;
  EXPECT_TRUE(decoder.Decode(decoded));
  EXPECT_TRUE(decoded.empty());
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, RoundTripsNumericNodeIds) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::NodeId{});
  encoder.Encode(opcua::NodeId{255, 2});
  encoder.Encode(opcua::NodeId{70000, 513});
  encoder.Encode(opcua::ExpandedNodeId{opcua::NodeId{42, 2}, "urn:test", 7});

  Decoder decoder{bytes};
  opcua::NodeId null_id;
  opcua::NodeId small_id;
  opcua::NodeId large_id;
  opcua::ExpandedNodeId expanded_id;
  EXPECT_TRUE(decoder.Decode(null_id));
  EXPECT_TRUE(decoder.Decode(small_id));
  EXPECT_TRUE(decoder.Decode(large_id));
  EXPECT_TRUE(decoder.Decode(expanded_id));

  EXPECT_TRUE(null_id.is_null());
  EXPECT_EQ(small_id, (opcua::NodeId{255, 2}));
  EXPECT_EQ(large_id, (opcua::NodeId{70000, 513}));
  EXPECT_EQ(expanded_id,
            (opcua::ExpandedNodeId{opcua::NodeId{42, 2}, "urn:test", 7}));
  EXPECT_TRUE(decoder.consumed());
}

// OPC UA Part 6 §5.2.2.9 Table 6,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.9: TwoByte
// NodeIds are the encoding byte 0x00 followed by a one-byte identifier — a
// null NodeId is TWO bytes on the wire (identifier 0), never the encoding
// byte alone. Byte-exact so the encoder cannot drift back to the pre-2026-07
// one-byte null form that broke interop with spec-conforming stacks.
TEST(CodecUtilsTest, EncodesTwoByteNodeIdsPerSpec) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::NodeId{});        // null -> 00 00
  encoder.Encode(opcua::NodeId{7, 0});    // ns 0, id 7 -> 00 07
  encoder.Encode(opcua::NodeId{255, 0});  // ns 0, id 255 -> 00 ff
  encoder.Encode(opcua::NodeId{256, 0});  // FourByte -> 01 00 00 01
  ASSERT_EQ(bytes.size(), 10u);
  const std::vector<std::uint8_t> expected{0x00, 0x00, 0x00, 0x07, 0x00,
                                           0xff, 0x01, 0x00, 0x00, 0x01};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[i]), expected[i]) << "byte " << i;
  }

  Decoder decoder{bytes};
  opcua::NodeId null_id;
  opcua::NodeId two_byte_id;
  opcua::NodeId max_two_byte_id;
  opcua::NodeId four_byte_id;
  EXPECT_TRUE(decoder.Decode(null_id));
  EXPECT_TRUE(decoder.Decode(two_byte_id));
  EXPECT_TRUE(decoder.Decode(max_two_byte_id));
  EXPECT_TRUE(decoder.Decode(four_byte_id));
  EXPECT_TRUE(null_id.is_null());
  EXPECT_EQ(two_byte_id, (opcua::NodeId{7, 0}));
  EXPECT_EQ(max_two_byte_id, (opcua::NodeId{255, 0}));
  EXPECT_EQ(four_byte_id, (opcua::NodeId{256, 0}));
  EXPECT_TRUE(decoder.consumed());
}

// OPC UA Part 6 §5.2.2.15: an ExtensionObject with no body encodes as the type
// id followed by encoding byte 0x00 and NO length; a present body uses 0x01 +
// an Int32 length. Getting the empty case wrong (0x01 + 0) differs from the
// null-body form the rest of the stack emits and is what an absent
// RequestHeader additionalHeader relies on.
TEST(CodecUtilsTest, EncodesAbsentExtensionObjectBodyAsZeroEncodingByte) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::ExtensionObject{});
  // Null type id (TwoByte NodeId 0) + encoding byte 0x00, nothing more.
  ASSERT_EQ(bytes.size(), 3u);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0x00);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[1]), 0x00);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[2]), 0x00);

  std::vector<char> with_body;
  Encoder body_encoder{with_body};
  body_encoder.Encode(
      opcua::ExtensionObject{opcua::ExpandedNodeId{opcua::NodeId{17537, 0}},
                             opcua::ByteString{'a', 'b'}});
  // Four-byte type id (01 00 81 44), encoding byte 0x01, Int32 length 2, body.
  ASSERT_EQ(with_body.size(), 4u + 1u + 4u + 2u);
  EXPECT_EQ(static_cast<std::uint8_t>(with_body[4]), 0x01);
  EXPECT_EQ(static_cast<std::uint8_t>(with_body[5]), 0x02);

  Decoder decoder{bytes};
  opcua::ExtensionObject decoded;
  EXPECT_TRUE(decoder.Decode(decoded));
  EXPECT_TRUE(decoder.consumed());
  EXPECT_TRUE(decoded.data_type_id().node_id().is_null());
}

// The ExpandedNodeId flavor of the same rule: the TwoByte identifier byte is
// present and the namespace-uri/server-index flag bits still apply.
TEST(CodecUtilsTest, EncodesTwoByteExpandedNodeIdsPerSpec) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::ExpandedNodeId{opcua::NodeId{}});
  encoder.Encode(opcua::ExpandedNodeId{opcua::NodeId{9, 0}, "urn:test", 0});
  ASSERT_GE(bytes.size(), 4u);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0x00);  // TwoByte, no flags
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[1]), 0x00);  // identifier 0
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[2]), 0x80);  // TwoByte + ns uri
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[3]), 0x09);  // identifier 9

  Decoder decoder{bytes};
  opcua::ExpandedNodeId null_id;
  opcua::ExpandedNodeId with_uri;
  EXPECT_TRUE(decoder.Decode(null_id));
  EXPECT_TRUE(decoder.Decode(with_uri));
  EXPECT_TRUE(null_id.node_id().is_null());
  EXPECT_EQ(with_uri,
            (opcua::ExpandedNodeId{opcua::NodeId{9, 0}, "urn:test", 0}));
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, RoundTripsStringAndOpaqueNodeIds) {
  const opcua::ByteString opaque_id{'o', 'p', 'a', 'q', 'u', 'e'};

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::NodeId{opcua::String{"StringNode"}, 4});
  encoder.Encode(opcua::NodeId{opaque_id, 5});
  encoder.Encode(opcua::ExpandedNodeId{
      opcua::NodeId{opcua::String{"ExpandedStringNode"}, 6}, "urn:string", 8});
  encoder.Encode(
      opcua::ExpandedNodeId{opcua::NodeId{opaque_id, 7}, "urn:opaque", 9});

  Decoder decoder{bytes};
  opcua::NodeId string_id;
  opcua::NodeId opaque_node_id;
  opcua::ExpandedNodeId expanded_string_id;
  opcua::ExpandedNodeId expanded_opaque_id;
  EXPECT_TRUE(decoder.Decode(string_id));
  EXPECT_TRUE(decoder.Decode(opaque_node_id));
  EXPECT_TRUE(decoder.Decode(expanded_string_id));
  EXPECT_TRUE(decoder.Decode(expanded_opaque_id));

  EXPECT_EQ(string_id, (opcua::NodeId{opcua::String{"StringNode"}, 4}));
  EXPECT_EQ(opaque_node_id, (opcua::NodeId{opaque_id, 5}));
  EXPECT_EQ(expanded_string_id,
            (opcua::ExpandedNodeId{
                opcua::NodeId{opcua::String{"ExpandedStringNode"}, 6},
                "urn:string", 8}));
  EXPECT_EQ(
      expanded_opaque_id,
      (opcua::ExpandedNodeId{opcua::NodeId{opaque_id, 7}, "urn:opaque", 9}));
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, RoundTripsExtensionObjectsAndMessages) {
  const std::vector<char> body{'x', 'y', 'z'};

  std::vector<char> extension_bytes;
  Encoder extension_encoder{extension_bytes};
  extension_encoder.Encode(
      EncodedExtensionObject{.type_id = 324, .body = body});

  Decoder extension_decoder{extension_bytes};
  DecodedExtensionObject extension;
  ASSERT_TRUE(extension_decoder.Decode(extension));
  EXPECT_EQ(extension.type_id, 324u);
  EXPECT_EQ(extension.encoding, 0x01);
  EXPECT_EQ(extension.body, body);
  EXPECT_TRUE(extension_decoder.consumed());

  std::vector<char> message_bytes;
  Encoder message_encoder{message_bytes};
  AppendMessage(message_encoder, 629, body);
  Decoder message_decoder{message_bytes};
  const auto message = ReadMessage(message_decoder);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->first, 629u);
  EXPECT_TRUE(std::equal(message->second.begin(), message->second.end(),
                         body.begin(), body.end()));
}

TEST(CodecUtilsTest, RoundTripsVariants) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  const auto date_time = opcua::DateTime::FromDeltaSinceWindowsEpoch(
      opcua::Duration::FromMicroseconds(1234567));
  const opcua::ExtensionObject extension_object{
      opcua::ExpandedNodeId{opcua::NodeId{122, 2}, "urn:test", 3},
      opcua::ByteString{'x', 'y'}};
  encoder.Encode(opcua::Variant{});
  encoder.Encode(opcua::Variant{true});
  encoder.Encode(opcua::Variant{opcua::Int8{-7}});
  encoder.Encode(opcua::Variant{opcua::UInt8{8}});
  encoder.Encode(opcua::Variant{opcua::Int16{-16}});
  encoder.Encode(opcua::Variant{opcua::UInt16{16}});
  encoder.Encode(opcua::Variant{opcua::Int32{-32}});
  encoder.Encode(opcua::Variant{opcua::UInt32{32}});
  encoder.Encode(opcua::Variant{opcua::Int64{-64}});
  encoder.Encode(opcua::Variant{opcua::UInt64{64}});
  encoder.Encode(opcua::Variant{opcua::Double{3.5}});
  encoder.Encode(opcua::Variant{opcua::ByteString{'a', 'b'}});
  encoder.Encode(opcua::Variant{opcua::String{"abc"}});
  encoder.Encode(opcua::Variant{opcua::QualifiedName{"BrowseName", 2}});
  encoder.Encode(opcua::Variant{
      opcua::ToLocalizedText(std::u16string_view{u"DisplayName"})});
  encoder.Encode(opcua::Variant{opcua::NodeId{42, 2}});
  encoder.Encode(opcua::Variant{
      opcua::ExpandedNodeId{opcua::NodeId{43, 2}, "urn:test", 4}});
  encoder.Encode(opcua::Variant{extension_object});
  encoder.Encode(opcua::Variant{date_time});
  encoder.Encode(opcua::Variant{std::vector<std::monostate>(2)});
  encoder.Encode(opcua::Variant{std::vector<bool>{true, false}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Int8>{-1, 2}});
  encoder.Encode(opcua::Variant{std::vector<opcua::UInt8>{3, 4}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Int16>{-5, 6}});
  encoder.Encode(opcua::Variant{std::vector<opcua::UInt16>{7, 8}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Int32>{-9, 10}});
  encoder.Encode(opcua::Variant{std::vector<opcua::UInt32>{11, 12}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Int64>{-13, 14}});
  encoder.Encode(opcua::Variant{std::vector<opcua::UInt64>{15, 16}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Double>{1.5, 2.5}});
  encoder.Encode(
      opcua::Variant{std::vector<opcua::ByteString>{{'c'}, {'d', 'e'}}});
  encoder.Encode(opcua::Variant{std::vector<opcua::String>{"fg", "hi"}});
  encoder.Encode(opcua::Variant{
      std::vector<opcua::QualifiedName>{{"Name1", 1}, {"Name2", 2}}});
  encoder.Encode(opcua::Variant{std::vector<opcua::LocalizedText>{
      opcua::ToLocalizedText(std::u16string_view{u"One"}),
      opcua::ToLocalizedText(std::u16string_view{u"Two"})}});
  encoder.Encode(opcua::Variant{std::vector<opcua::NodeId>{{21, 2}, {22, 3}}});
  encoder.Encode(opcua::Variant{std::vector<opcua::ExpandedNodeId>{
      {opcua::NodeId{23, 2}, "urn:a", 1}, {opcua::NodeId{24, 3}, "urn:b", 2}}});
  encoder.Encode(
      opcua::Variant{std::vector<opcua::ExtensionObject>{extension_object}});

  Decoder decoder{bytes};
  std::vector<opcua::Variant> decoded(37);
  for (auto& variant : decoded) {
    EXPECT_TRUE(decoder.Decode(variant));
  }

  EXPECT_TRUE(decoded[0].is_null());
  EXPECT_EQ(decoded[1].get<bool>(), true);
  EXPECT_EQ(decoded[2].get<opcua::Int8>(), -7);
  EXPECT_EQ(decoded[3].get<opcua::UInt8>(), 8);
  EXPECT_EQ(decoded[4].get<opcua::Int16>(), -16);
  EXPECT_EQ(decoded[5].get<opcua::UInt16>(), 16);
  EXPECT_EQ(decoded[6].get<opcua::Int32>(), -32);
  EXPECT_EQ(decoded[7].get<opcua::UInt32>(), 32u);
  EXPECT_EQ(decoded[8].get<opcua::Int64>(), -64);
  EXPECT_EQ(decoded[9].get<opcua::UInt64>(), 64u);
  EXPECT_DOUBLE_EQ(decoded[10].get<opcua::Double>(), 3.5);
  EXPECT_EQ(decoded[11].get<opcua::ByteString>(),
            (opcua::ByteString{'a', 'b'}));
  EXPECT_EQ(decoded[12].get<opcua::String>(), "abc");
  EXPECT_EQ(decoded[13].get<opcua::QualifiedName>(),
            (opcua::QualifiedName{"BrowseName", 2}));
  EXPECT_EQ(decoded[14].get<opcua::LocalizedText>(),
            opcua::ToLocalizedText(std::u16string_view{u"DisplayName"}));
  EXPECT_EQ(decoded[15].get<opcua::NodeId>(), (opcua::NodeId{42, 2}));
  EXPECT_EQ(decoded[16].get<opcua::ExpandedNodeId>(),
            (opcua::ExpandedNodeId{opcua::NodeId{43, 2}, "urn:test", 4}));
  EXPECT_EQ(decoded[17].get<opcua::ExtensionObject>().data_type_id(),
            extension_object.data_type_id());
  EXPECT_EQ(std::any_cast<opcua::ByteString>(
                decoded[17].get<opcua::ExtensionObject>().value()),
            (opcua::ByteString{'x', 'y'}));
  EXPECT_EQ(decoded[18].get<opcua::DateTime>(), date_time);
  EXPECT_EQ(decoded[19].get<std::vector<std::monostate>>().size(), 2u);
  EXPECT_EQ(decoded[20].get<std::vector<bool>>(),
            (std::vector<bool>{true, false}));
  EXPECT_EQ(decoded[21].get<std::vector<opcua::Int8>>(),
            (std::vector<opcua::Int8>{-1, 2}));
  EXPECT_EQ(decoded[22].get<std::vector<opcua::UInt8>>(),
            (std::vector<opcua::UInt8>{3, 4}));
  EXPECT_EQ(decoded[23].get<std::vector<opcua::Int16>>(),
            (std::vector<opcua::Int16>{-5, 6}));
  EXPECT_EQ(decoded[24].get<std::vector<opcua::UInt16>>(),
            (std::vector<opcua::UInt16>{7, 8}));
  EXPECT_EQ(decoded[25].get<std::vector<opcua::Int32>>(),
            (std::vector<opcua::Int32>{-9, 10}));
  EXPECT_EQ(decoded[26].get<std::vector<opcua::UInt32>>(),
            (std::vector<opcua::UInt32>{11, 12}));
  EXPECT_EQ(decoded[27].get<std::vector<opcua::Int64>>(),
            (std::vector<opcua::Int64>{-13, 14}));
  EXPECT_EQ(decoded[28].get<std::vector<opcua::UInt64>>(),
            (std::vector<opcua::UInt64>{15, 16}));
  EXPECT_EQ(decoded[29].get<std::vector<opcua::Double>>(),
            (std::vector<opcua::Double>{1.5, 2.5}));
  EXPECT_EQ(decoded[30].get<std::vector<opcua::ByteString>>(),
            (std::vector<opcua::ByteString>{{'c'}, {'d', 'e'}}));
  EXPECT_EQ(decoded[31].get<std::vector<opcua::String>>(),
            (std::vector<opcua::String>{"fg", "hi"}));
  EXPECT_EQ(decoded[32].get<std::vector<opcua::QualifiedName>>(),
            (std::vector<opcua::QualifiedName>{{"Name1", 1}, {"Name2", 2}}));
  EXPECT_EQ(decoded[33].get<std::vector<opcua::LocalizedText>>(),
            (std::vector<opcua::LocalizedText>{
                opcua::ToLocalizedText(std::u16string_view{u"One"}),
                opcua::ToLocalizedText(std::u16string_view{u"Two"})}));
  EXPECT_EQ(decoded[34].get<std::vector<opcua::NodeId>>(),
            (std::vector<opcua::NodeId>{{21, 2}, {22, 3}}));
  EXPECT_EQ(
      decoded[35].get<std::vector<opcua::ExpandedNodeId>>(),
      (std::vector<opcua::ExpandedNodeId>{{opcua::NodeId{23, 2}, "urn:a", 1},
                                          {opcua::NodeId{24, 3}, "urn:b", 2}}));
  ASSERT_EQ(decoded[36].get<std::vector<opcua::ExtensionObject>>().size(), 1u);
  EXPECT_EQ(
      decoded[36].get<std::vector<opcua::ExtensionObject>>()[0].data_type_id(),
      extension_object.data_type_id());
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, RejectsTruncatedPayloads) {
  std::vector<char> truncated_string;
  Encoder truncated_encoder{truncated_string};
  truncated_encoder.Encode(std::int32_t{4});
  truncated_string.push_back('o');

  Decoder truncated_decoder{truncated_string};
  std::string value;
  EXPECT_FALSE(truncated_decoder.Decode(value));

  std::vector<char> invalid_message{0x03};
  Decoder invalid_decoder{invalid_message};
  EXPECT_FALSE(ReadMessage(invalid_decoder).has_value());
}

TEST(CodecUtilsTest, RejectsArrayVariantWithCountExceedingBuffer) {
  // Int32 array (built-in type 6 | array flag 0x80) claiming ~2e9 elements with
  // no element bytes following: the decoder must reject it, not try to reserve.
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::uint8_t{0x86});
  encoder.Encode(std::int32_t{2000000000});

  Decoder decoder{bytes};
  opcua::Variant value;
  EXPECT_FALSE(decoder.Decode(value));
}

TEST(CodecUtilsTest, RejectsNullArrayVariantWithHugeCount) {
  // A Null (EMPTY) array whose elements carry no bytes, with a count beyond the
  // safety cap, must be rejected rather than allocated.
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::uint8_t{0x80});
  encoder.Encode(std::int32_t{2000000000});

  Decoder decoder{bytes};
  opcua::Variant value;
  EXPECT_FALSE(decoder.Decode(value));
}

TEST(CodecUtilsTest, DecodesSmallNullArrayVariant) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::uint8_t{0x80});
  encoder.Encode(std::int32_t{3});

  Decoder decoder{bytes};
  opcua::Variant value;
  ASSERT_TRUE(decoder.Decode(value));
  EXPECT_TRUE(decoder.consumed());
}

// The built-in types added when the Variant was widened to the spec's full
// BuiltInType set. Encoded as scalars and as arrays, since the two paths are
// separate switch arms.
TEST(CodecUtilsTest, RoundTripsBuiltInTypesBeyondTheLegacySet) {
  const opcua::Guid guid{
      .data1 = 0x72962B91,
      .data2 = 0xFA75,
      .data3 = 0x4AE6,
      .data4 = {0x8D, 0x28, 0xB4, 0x04, 0xDC, 0x7D, 0xAF, 0x63}};
  const opcua::XmlElement xml{"<a b=\"1\"/>"};
  const opcua::Status status{opcua::StatusCode::Bad_NodeIdUnknown};

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::Variant{opcua::Float{1.5f}});
  encoder.Encode(opcua::Variant{guid});
  encoder.Encode(opcua::Variant{xml});
  encoder.Encode(opcua::Variant{status});
  encoder.Encode(opcua::Variant{std::vector<opcua::Float>{2.5f, -3.5f}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Guid>{guid, opcua::Guid{}}});
  encoder.Encode(opcua::Variant{std::vector<opcua::XmlElement>{xml}});
  encoder.Encode(opcua::Variant{std::vector<opcua::Status>{status}});
  // Arrays of DateTime used to be encoded as an empty Int32, silently dropping
  // every element.
  const auto date_time = opcua::DateTime::FromDeltaSinceWindowsEpoch(
      opcua::Duration::FromMicroseconds(98765));
  encoder.Encode(opcua::Variant{std::vector<opcua::DateTime>{date_time}});

  Decoder decoder{bytes};
  std::vector<opcua::Variant> decoded(9);
  for (auto& variant : decoded)
    ASSERT_TRUE(decoder.Decode(variant));

  EXPECT_EQ(decoded[0].get<opcua::Float>(), 1.5f);
  EXPECT_EQ(decoded[1].get<opcua::Guid>(), guid);
  EXPECT_EQ(decoded[2].get<opcua::XmlElement>(), xml);
  EXPECT_EQ(decoded[3].get<opcua::Status>(), status);
  EXPECT_EQ(decoded[4].get<std::vector<opcua::Float>>(),
            (std::vector<opcua::Float>{2.5f, -3.5f}));
  EXPECT_EQ(decoded[5].get<std::vector<opcua::Guid>>(),
            (std::vector<opcua::Guid>{guid, opcua::Guid{}}));
  EXPECT_EQ(decoded[6].get<std::vector<opcua::XmlElement>>(),
            (std::vector<opcua::XmlElement>{xml}));
  EXPECT_EQ(decoded[7].get<std::vector<opcua::Status>>(),
            (std::vector<opcua::Status>{status}));
  EXPECT_EQ(decoded[8].get<std::vector<opcua::DateTime>>(),
            (std::vector<opcua::DateTime>{date_time}));
  EXPECT_TRUE(decoder.consumed());
}

// The Variant wire id is the spec BuiltInType id, not an opcuapp-internal
// number. Pinning the encoding-mask byte here is what keeps a reordering of
// Variant::Type from silently changing the wire. OPC UA Part 6 §5.1.2,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2
TEST(CodecUtilsTest, VariantEncodingMaskCarriesTheSpecBuiltInTypeId) {
  const auto encode = [](const opcua::Variant& value) {
    std::vector<char> bytes;
    Encoder encoder{bytes};
    encoder.Encode(value);
    return static_cast<std::uint8_t>(bytes.at(0));
  };

  EXPECT_EQ(encode(opcua::Variant{}), 0);
  EXPECT_EQ(encode(opcua::Variant{true}), 1);
  EXPECT_EQ(encode(opcua::Variant{opcua::Float{1.0f}}), 10);
  EXPECT_EQ(encode(opcua::Variant{opcua::Double{1.0}}), 11);
  EXPECT_EQ(encode(opcua::Variant{opcua::String{"x"}}), 12);
  EXPECT_EQ(encode(opcua::Variant{opcua::Guid{}}), 14);
  EXPECT_EQ(encode(opcua::Variant{opcua::XmlElement{"<x/>"}}), 16);
  EXPECT_EQ(encode(opcua::Variant{opcua::Status{opcua::StatusCode::Good}}), 19);
  // Bit 7 marks an array of the same built-in type.
  EXPECT_EQ(encode(opcua::Variant{std::vector<opcua::Float>{1.0f}}), 0x80 | 10);
}

// A DiagnosticInfo nests another DiagnosticInfo and mixes its optional fields;
// the mask bit order (LocalizedText before Locale) differs from the payload
// order (Locale before LocalizedText), which is easy to get backwards.
TEST(CodecUtilsTest, RoundTripsNestedDiagnosticInfo) {
  DiagnosticInfo inner;
  inner.symbolic_id = 7;
  inner.additional_info = "inner";

  DiagnosticInfo info;
  info.namespace_uri = 1;
  info.locale = 2;
  info.localized_text = 3;
  info.additional_info = "outer";
  info.inner_status_code = Status{StatusCode::Bad_NodeIdUnknown};
  info.inner_diagnostic_info =
      std::make_shared<const DiagnosticInfo>(std::move(inner));

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(info);
  encoder.Encode(DiagnosticInfo{});

  Decoder decoder{bytes};
  DiagnosticInfo decoded;
  DiagnosticInfo decoded_empty;
  ASSERT_TRUE(decoder.Decode(decoded));
  ASSERT_TRUE(decoder.Decode(decoded_empty));
  EXPECT_EQ(decoded, info);
  EXPECT_TRUE(decoded_empty.empty());
  EXPECT_TRUE(decoder.consumed());
  // An empty DiagnosticInfo is a single zero mask byte.
  EXPECT_EQ(bytes.back(), 0);
}

// A DataValue carries a Variant, which can in turn carry a DataValue.
TEST(CodecUtilsTest, RoundTripsDataValueAndNestedVariant) {
  const auto source_timestamp = opcua::DateTime::FromDeltaSinceWindowsEpoch(
      opcua::Duration::FromMicroseconds(4242));
  DataValue inner;
  inner.value = opcua::Variant{opcua::Int32{-11}};
  inner.status_code = StatusCode::Uncertain;
  inner.source_timestamp = source_timestamp;

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(inner);
  encoder.Encode(opcua::Variant{std::make_shared<const DataValue>(inner)});
  encoder.Encode(opcua::Variant{std::make_shared<const opcua::Variant>(
      opcua::Variant{String{"nested"}})});

  Decoder decoder{bytes};
  DataValue decoded_data_value;
  opcua::Variant decoded_nested_data_value;
  opcua::Variant decoded_nested_variant;
  ASSERT_TRUE(decoder.Decode(decoded_data_value));
  ASSERT_TRUE(decoder.Decode(decoded_nested_data_value));
  ASSERT_TRUE(decoder.Decode(decoded_nested_variant));

  EXPECT_EQ(decoded_data_value.value.get<opcua::Int32>(), -11);
  EXPECT_EQ(decoded_data_value.status_code, StatusCode::Uncertain);
  EXPECT_EQ(decoded_data_value.source_timestamp, source_timestamp);

  ASSERT_EQ(decoded_nested_data_value.type(), opcua::Variant::DATA_VALUE);
  EXPECT_EQ(decoded_nested_data_value.get<std::shared_ptr<const DataValue>>()
                ->value.get<opcua::Int32>(),
            -11);
  ASSERT_EQ(decoded_nested_variant.type(), opcua::Variant::VARIANT);
  EXPECT_EQ(decoded_nested_variant.get<std::shared_ptr<const opcua::Variant>>()
                ->get<String>(),
            "nested");
  EXPECT_TRUE(decoder.consumed());
}

// A peer may send sub-100ns timestamps that opcuapp cannot represent. Dropping
// the extra fields keeps the rest of the frame parseable; rejecting the whole
// DataValue would desynchronise the stream.
TEST(CodecUtilsTest, DecodesDataValuePicosecondsAndDiscardsThem) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  // Value + SourceTimestamp + SourcePicoseconds + ServerTimestamp.
  encoder.Encode(std::uint8_t{0x01 | 0x04 | 0x10 | 0x08});
  encoder.Encode(opcua::Variant{opcua::Int32{5}});
  encoder.Encode(opcua::DateTime::FromInternalValue(111));
  encoder.Encode(std::uint16_t{999});
  encoder.Encode(opcua::DateTime::FromInternalValue(222));

  Decoder decoder{bytes};
  DataValue decoded;
  ASSERT_TRUE(decoder.Decode(decoded));
  EXPECT_EQ(decoded.value.get<opcua::Int32>(), 5);
  EXPECT_EQ(decoded.source_timestamp, opcua::DateTime::FromInternalValue(111));
  EXPECT_EQ(decoded.server_timestamp, opcua::DateTime::FromInternalValue(222));
  EXPECT_TRUE(decoder.consumed());
}

// Bit 6 of the encoding mask marks a matrix (ArrayDimensions), which opcuapp
// does not model. Decoding it as a flat array would misread the rest of the
// frame, so it is rejected outright.
TEST(CodecUtilsTest, RejectsMatrixVariant) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(static_cast<std::uint8_t>(0x80 | 0x40 | 6));
  encoder.Encode(std::int32_t{0});

  Decoder decoder{bytes};
  opcua::Variant value;
  EXPECT_FALSE(decoder.Decode(value));
}

TEST(CodecUtilsTest, RejectsUnknownBuiltInTypeId) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::uint8_t{26});  // One past DiagnosticInfo.

  Decoder decoder{bytes};
  opcua::Variant value;
  EXPECT_FALSE(decoder.Decode(value));
}

}  // namespace
}  // namespace opcua::binary
