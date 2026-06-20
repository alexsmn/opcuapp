#include "opcua/binary/codec_utils.h"

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
  encoder.Encode(scada::QualifiedName{"BrowseName", 2});
  encoder.Encode(scada::ToLocalizedText(
      std::u16string_view{u"DisplayName"}));
  encoder.Encode(scada::ByteString{'a', 'b', 'c'});
  encoder.Encode(std::int32_t{-1});
  encoder.Encode(std::int32_t{-1});

  Decoder decoder{bytes};
  std::string string_value;
  scada::QualifiedName qualified_name;
  scada::LocalizedText localized_text;
  scada::ByteString byte_string;
  std::string null_string = "sentinel";
  scada::ByteString null_bytes = {'x'};
  EXPECT_TRUE(decoder.Decode(string_value));
  EXPECT_TRUE(decoder.Decode(qualified_name));
  EXPECT_TRUE(decoder.Decode(localized_text));
  EXPECT_TRUE(decoder.Decode(byte_string));
  EXPECT_TRUE(decoder.Decode(null_string));
  EXPECT_TRUE(decoder.Decode(null_bytes));

  EXPECT_EQ(string_value, "opc.tcp://localhost:4840");
  EXPECT_EQ(qualified_name, (scada::QualifiedName{"BrowseName", 2}));
  EXPECT_EQ(localized_text,
            scada::ToLocalizedText(std::u16string_view{u"DisplayName"}));
  EXPECT_EQ(byte_string, (scada::ByteString{'a', 'b', 'c'}));
  EXPECT_TRUE(null_string.empty());
  EXPECT_TRUE(null_bytes.empty());
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, EncodesEmptyLocalizedTextAsMaskOnly) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(scada::LocalizedText{});

  ASSERT_EQ(bytes.size(), 1u);
  EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0u);

  Decoder decoder{bytes};
  scada::LocalizedText decoded;
  EXPECT_TRUE(decoder.Decode(decoded));
  EXPECT_TRUE(decoded.empty());
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, RoundTripsNumericNodeIds) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(scada::NodeId{});
  encoder.Encode(scada::NodeId{255, 2});
  encoder.Encode(scada::NodeId{70000, 513});
  encoder.Encode(
      scada::ExpandedNodeId{scada::NodeId{42, 2}, "urn:test", 7});

  Decoder decoder{bytes};
  scada::NodeId null_id;
  scada::NodeId small_id;
  scada::NodeId large_id;
  scada::ExpandedNodeId expanded_id;
  EXPECT_TRUE(decoder.Decode(null_id));
  EXPECT_TRUE(decoder.Decode(small_id));
  EXPECT_TRUE(decoder.Decode(large_id));
  EXPECT_TRUE(decoder.Decode(expanded_id));

  EXPECT_TRUE(null_id.is_null());
  EXPECT_EQ(small_id, (scada::NodeId{255, 2}));
  EXPECT_EQ(large_id, (scada::NodeId{70000, 513}));
  EXPECT_EQ(expanded_id,
            (scada::ExpandedNodeId{scada::NodeId{42, 2}, "urn:test", 7}));
  EXPECT_TRUE(decoder.consumed());
}

TEST(CodecUtilsTest, RoundTripsStringAndOpaqueNodeIds) {
  const scada::ByteString opaque_id{'o', 'p', 'a', 'q', 'u', 'e'};

  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(scada::NodeId{scada::String{"StringNode"}, 4});
  encoder.Encode(scada::NodeId{opaque_id, 5});
  encoder.Encode(scada::ExpandedNodeId{
      scada::NodeId{scada::String{"ExpandedStringNode"}, 6},
      "urn:string", 8});
  encoder.Encode(scada::ExpandedNodeId{
      scada::NodeId{opaque_id, 7}, "urn:opaque", 9});

  Decoder decoder{bytes};
  scada::NodeId string_id;
  scada::NodeId opaque_node_id;
  scada::ExpandedNodeId expanded_string_id;
  scada::ExpandedNodeId expanded_opaque_id;
  EXPECT_TRUE(decoder.Decode(string_id));
  EXPECT_TRUE(decoder.Decode(opaque_node_id));
  EXPECT_TRUE(decoder.Decode(expanded_string_id));
  EXPECT_TRUE(decoder.Decode(expanded_opaque_id));

  EXPECT_EQ(string_id, (scada::NodeId{scada::String{"StringNode"}, 4}));
  EXPECT_EQ(opaque_node_id, (scada::NodeId{opaque_id, 5}));
  EXPECT_EQ(expanded_string_id,
            (scada::ExpandedNodeId{
                scada::NodeId{scada::String{"ExpandedStringNode"}, 6},
                "urn:string", 8}));
  EXPECT_EQ(expanded_opaque_id,
            (scada::ExpandedNodeId{
                scada::NodeId{opaque_id, 7}, "urn:opaque", 9}));
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
      base::Time::FromDeltaSinceWindowsEpoch(
          base::TimeDelta::FromMicroseconds(1234567));
  const scada::ExtensionObject extension_object{
      scada::ExpandedNodeId{scada::NodeId{122, 2}, "urn:test", 3},
      scada::ByteString{'x', 'y'}};
  encoder.Encode(scada::Variant{});
  encoder.Encode(scada::Variant{true});
  encoder.Encode(scada::Variant{scada::Int8{-7}});
  encoder.Encode(scada::Variant{scada::UInt8{8}});
  encoder.Encode(scada::Variant{scada::Int16{-16}});
  encoder.Encode(scada::Variant{scada::UInt16{16}});
  encoder.Encode(scada::Variant{scada::Int32{-32}});
  encoder.Encode(scada::Variant{scada::UInt32{32}});
  encoder.Encode(scada::Variant{scada::Int64{-64}});
  encoder.Encode(scada::Variant{scada::UInt64{64}});
  encoder.Encode(scada::Variant{scada::Double{3.5}});
  encoder.Encode(scada::Variant{scada::ByteString{'a', 'b'}});
  encoder.Encode(scada::Variant{scada::String{"abc"}});
  encoder.Encode(scada::Variant{scada::QualifiedName{"BrowseName", 2}});
  encoder.Encode(scada::Variant{scada::ToLocalizedText(
      std::u16string_view{u"DisplayName"})});
  encoder.Encode(scada::Variant{scada::NodeId{42, 2}});
  encoder.Encode(
      scada::Variant{scada::ExpandedNodeId{scada::NodeId{43, 2}, "urn:test", 4}});
  encoder.Encode(scada::Variant{extension_object});
  encoder.Encode(scada::Variant{date_time});
  encoder.Encode(scada::Variant{std::vector<std::monostate>(2)});
  encoder.Encode(scada::Variant{std::vector<bool>{true, false}});
  encoder.Encode(scada::Variant{std::vector<scada::Int8>{-1, 2}});
  encoder.Encode(scada::Variant{std::vector<scada::UInt8>{3, 4}});
  encoder.Encode(scada::Variant{std::vector<scada::Int16>{-5, 6}});
  encoder.Encode(scada::Variant{std::vector<scada::UInt16>{7, 8}});
  encoder.Encode(scada::Variant{std::vector<scada::Int32>{-9, 10}});
  encoder.Encode(scada::Variant{std::vector<scada::UInt32>{11, 12}});
  encoder.Encode(scada::Variant{std::vector<scada::Int64>{-13, 14}});
  encoder.Encode(scada::Variant{std::vector<scada::UInt64>{15, 16}});
  encoder.Encode(scada::Variant{std::vector<scada::Double>{1.5, 2.5}});
  encoder.Encode(
      scada::Variant{std::vector<scada::ByteString>{{'c'}, {'d', 'e'}}});
  encoder.Encode(
      scada::Variant{std::vector<scada::String>{"fg", "hi"}});
  encoder.Encode(scada::Variant{
      std::vector<scada::QualifiedName>{{"Name1", 1}, {"Name2", 2}}});
  encoder.Encode(scada::Variant{std::vector<scada::LocalizedText>{
      scada::ToLocalizedText(std::u16string_view{u"One"}),
      scada::ToLocalizedText(std::u16string_view{u"Two"})}});
  encoder.Encode(scada::Variant{
      std::vector<scada::NodeId>{{21, 2}, {22, 3}}});
  encoder.Encode(scada::Variant{std::vector<scada::ExpandedNodeId>{
      {scada::NodeId{23, 2}, "urn:a", 1},
      {scada::NodeId{24, 3}, "urn:b", 2}}});
  encoder.Encode(scada::Variant{
      std::vector<scada::ExtensionObject>{extension_object}});

  Decoder decoder{bytes};
  std::vector<scada::Variant> decoded(37);
  for (auto& variant : decoded) {
    EXPECT_TRUE(decoder.Decode(variant));
  }

  EXPECT_TRUE(decoded[0].is_null());
  EXPECT_EQ(decoded[1].get<bool>(), true);
  EXPECT_EQ(decoded[2].get<scada::Int8>(), -7);
  EXPECT_EQ(decoded[3].get<scada::UInt8>(), 8);
  EXPECT_EQ(decoded[4].get<scada::Int16>(), -16);
  EXPECT_EQ(decoded[5].get<scada::UInt16>(), 16);
  EXPECT_EQ(decoded[6].get<scada::Int32>(), -32);
  EXPECT_EQ(decoded[7].get<scada::UInt32>(), 32u);
  EXPECT_EQ(decoded[8].get<scada::Int64>(), -64);
  EXPECT_EQ(decoded[9].get<scada::UInt64>(), 64u);
  EXPECT_DOUBLE_EQ(decoded[10].get<scada::Double>(), 3.5);
  EXPECT_EQ(decoded[11].get<scada::ByteString>(), (scada::ByteString{'a', 'b'}));
  EXPECT_EQ(decoded[12].get<scada::String>(), "abc");
  EXPECT_EQ(decoded[13].get<scada::QualifiedName>(),
            (scada::QualifiedName{"BrowseName", 2}));
  EXPECT_EQ(decoded[14].get<scada::LocalizedText>(),
            scada::ToLocalizedText(std::u16string_view{u"DisplayName"}));
  EXPECT_EQ(decoded[15].get<scada::NodeId>(), (scada::NodeId{42, 2}));
  EXPECT_EQ(decoded[16].get<scada::ExpandedNodeId>(),
            (scada::ExpandedNodeId{scada::NodeId{43, 2}, "urn:test", 4}));
  EXPECT_EQ(decoded[17].get<scada::ExtensionObject>().data_type_id(),
            extension_object.data_type_id());
  EXPECT_EQ(std::any_cast<scada::ByteString>(
                decoded[17].get<scada::ExtensionObject>().value()),
            (scada::ByteString{'x', 'y'}));
  EXPECT_EQ(decoded[18].get<scada::DateTime>(), date_time);
  EXPECT_EQ(decoded[19].get<std::vector<std::monostate>>().size(), 2u);
  EXPECT_EQ(decoded[20].get<std::vector<bool>>(), (std::vector<bool>{true, false}));
  EXPECT_EQ(decoded[21].get<std::vector<scada::Int8>>(),
            (std::vector<scada::Int8>{-1, 2}));
  EXPECT_EQ(decoded[22].get<std::vector<scada::UInt8>>(),
            (std::vector<scada::UInt8>{3, 4}));
  EXPECT_EQ(decoded[23].get<std::vector<scada::Int16>>(),
            (std::vector<scada::Int16>{-5, 6}));
  EXPECT_EQ(decoded[24].get<std::vector<scada::UInt16>>(),
            (std::vector<scada::UInt16>{7, 8}));
  EXPECT_EQ(decoded[25].get<std::vector<scada::Int32>>(),
            (std::vector<scada::Int32>{-9, 10}));
  EXPECT_EQ(decoded[26].get<std::vector<scada::UInt32>>(),
            (std::vector<scada::UInt32>{11, 12}));
  EXPECT_EQ(decoded[27].get<std::vector<scada::Int64>>(),
            (std::vector<scada::Int64>{-13, 14}));
  EXPECT_EQ(decoded[28].get<std::vector<scada::UInt64>>(),
            (std::vector<scada::UInt64>{15, 16}));
  EXPECT_EQ(decoded[29].get<std::vector<scada::Double>>(),
            (std::vector<scada::Double>{1.5, 2.5}));
  EXPECT_EQ(decoded[30].get<std::vector<scada::ByteString>>(),
            (std::vector<scada::ByteString>{{'c'}, {'d', 'e'}}));
  EXPECT_EQ(decoded[31].get<std::vector<scada::String>>(),
            (std::vector<scada::String>{"fg", "hi"}));
  EXPECT_EQ(decoded[32].get<std::vector<scada::QualifiedName>>(),
            (std::vector<scada::QualifiedName>{{"Name1", 1}, {"Name2", 2}}));
  EXPECT_EQ(decoded[33].get<std::vector<scada::LocalizedText>>(),
            (std::vector<scada::LocalizedText>{
                scada::ToLocalizedText(std::u16string_view{u"One"}),
                scada::ToLocalizedText(std::u16string_view{u"Two"})}));
  EXPECT_EQ(decoded[34].get<std::vector<scada::NodeId>>(),
            (std::vector<scada::NodeId>{{21, 2}, {22, 3}}));
  EXPECT_EQ(decoded[35].get<std::vector<scada::ExpandedNodeId>>(),
            (std::vector<scada::ExpandedNodeId>{
                {scada::NodeId{23, 2}, "urn:a", 1},
                {scada::NodeId{24, 3}, "urn:b", 2}}));
  ASSERT_EQ(decoded[36].get<std::vector<scada::ExtensionObject>>().size(), 1u);
  EXPECT_EQ(decoded[36].get<std::vector<scada::ExtensionObject>>()[0].data_type_id(),
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

}  // namespace
}  // namespace opcua::binary
