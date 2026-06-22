#include "opcua/transport/binary/codec_utils.h"

#include <algorithm>

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
  encoder.Encode(opcua::ToLocalizedText(
      std::u16string_view{u"DisplayName"}));
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
  encoder.Encode(
      opcua::ExpandedNodeId{opcua::NodeId{42, 2}, "urn:test", 7});

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

TEST(CodecUtilsTest, RoundTripsStringAndOpaqueNodeIds) {
  const opcua::ByteString opaque_id{'o', 'p', 'a', 'q', 'u', 'e'};

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(opcua::NodeId{opcua::String{"StringNode"}, 4});
  encoder.Encode(opcua::NodeId{opaque_id, 5});
  encoder.Encode(opcua::ExpandedNodeId{
      opcua::NodeId{opcua::String{"ExpandedStringNode"}, 6},
      "urn:string", 8});
  encoder.Encode(opcua::ExpandedNodeId{
      opcua::NodeId{opaque_id, 7}, "urn:opaque", 9});

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
  EXPECT_EQ(expanded_opaque_id,
            (opcua::ExpandedNodeId{
                opcua::NodeId{opaque_id, 7}, "urn:opaque", 9}));
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
  EXPECT_TRUE(std::equal(message->second.begin(),
                         message->second.end(), body.begin(),
                         body.end()));
}

TEST(CodecUtilsTest, RoundTripsVariants) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  const auto date_time =
      opcua::base::Time::FromDeltaSinceWindowsEpoch(
          opcua::base::TimeDelta::FromMicroseconds(1234567));
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
  encoder.Encode(opcua::Variant{opcua::ToLocalizedText(
      std::u16string_view{u"DisplayName"})});
  encoder.Encode(opcua::Variant{opcua::NodeId{42, 2}});
  encoder.Encode(
      opcua::Variant{opcua::ExpandedNodeId{opcua::NodeId{43, 2}, "urn:test", 4}});
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
  encoder.Encode(
      opcua::Variant{std::vector<opcua::String>{"fg", "hi"}});
  encoder.Encode(opcua::Variant{
      std::vector<opcua::QualifiedName>{{"Name1", 1}, {"Name2", 2}}});
  encoder.Encode(opcua::Variant{std::vector<opcua::LocalizedText>{
      opcua::ToLocalizedText(std::u16string_view{u"One"}),
      opcua::ToLocalizedText(std::u16string_view{u"Two"})}});
  encoder.Encode(opcua::Variant{
      std::vector<opcua::NodeId>{{21, 2}, {22, 3}}});
  encoder.Encode(opcua::Variant{std::vector<opcua::ExpandedNodeId>{
      {opcua::NodeId{23, 2}, "urn:a", 1},
      {opcua::NodeId{24, 3}, "urn:b", 2}}});
  encoder.Encode(opcua::Variant{
      std::vector<opcua::ExtensionObject>{extension_object}});

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
  EXPECT_EQ(decoded[11].get<opcua::ByteString>(), (opcua::ByteString{'a', 'b'}));
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
  EXPECT_EQ(decoded[20].get<std::vector<bool>>(), (std::vector<bool>{true, false}));
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
  EXPECT_EQ(decoded[35].get<std::vector<opcua::ExpandedNodeId>>(),
            (std::vector<opcua::ExpandedNodeId>{
                {opcua::NodeId{23, 2}, "urn:a", 1},
                {opcua::NodeId{24, 3}, "urn:b", 2}}));
  ASSERT_EQ(decoded[36].get<std::vector<opcua::ExtensionObject>>().size(), 1u);
  EXPECT_EQ(decoded[36].get<std::vector<opcua::ExtensionObject>>()[0].data_type_id(),
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

}  // namespace
}  // namespace opcua::binary
