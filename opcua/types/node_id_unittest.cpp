#include "opcua/types/node_id.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

constexpr Guid kGuid{.data1 = 0x72962B91,
                     .data2 = 0xFA75,
                     .data3 = 0x4AE6,
                     .data4 = {0x8D, 0x28, 0xB4, 0x04, 0xDC, 0x7D, 0xAF, 0x63}};

// The textual NodeId forms of OPC UA Part 3 §8.2 / Part 6 §5.3.1.10:
// `ns=<n>;` followed by `i=`, `s=`, `g=` or `b=`.
TEST(NodeIdTest, RoundTripsEveryIdentifierFormThroughText) {
  const NodeId numeric{42, 2};
  const NodeId string_id{String{"Some.Node"}, 3};
  const NodeId guid_id{kGuid, 4};
  const NodeId opaque{ByteString{'\x01', '\x02', '\xfe'}, 5};

  EXPECT_EQ(numeric.ToString(), "ns=2;i=42");
  EXPECT_EQ(string_id.ToString(), "ns=3;s=Some.Node");
  EXPECT_EQ(guid_id.ToString(), "ns=4;g=72962B91-FA75-4AE6-8D28-B404DC7DAF63");
  EXPECT_EQ(opaque.ToString(), "ns=5;b=AQL+");

  EXPECT_EQ(NodeId::FromString(numeric.ToString()), numeric);
  EXPECT_EQ(NodeId::FromString(string_id.ToString()), string_id);
  EXPECT_EQ(NodeId::FromString(guid_id.ToString()), guid_id);
  EXPECT_EQ(NodeId::FromString(opaque.ToString()), opaque);
}

TEST(NodeIdTest, ParsesGuidAndOpaqueInNamespaceZero) {
  const NodeId guid_id =
      NodeId::FromString("g=72962B91-FA75-4AE6-8D28-B404DC7DAF63");
  ASSERT_TRUE(guid_id.is_guid());
  EXPECT_EQ(guid_id.guid_id(), kGuid);
  EXPECT_EQ(guid_id.namespace_index(), 0);

  const NodeId opaque = NodeId::FromString("b=AQL+");
  ASSERT_EQ(opaque.type(), NodeIdType::Opaque);
  EXPECT_EQ(opaque.opaque_id(), (ByteString{'\x01', '\x02', '\xfe'}));
}

TEST(NodeIdTest, RejectsMalformedGuidAndOpaqueText) {
  EXPECT_TRUE(NodeId::FromString("g=not-a-guid").is_null());
  EXPECT_TRUE(NodeId::FromString("b=!!!!").is_null());
}

// Identifier kinds are distinct even when their text would collide.
TEST(NodeIdTest, GuidIdentifierIsDistinctFromTheSameTextAsAString) {
  const NodeId guid_id{kGuid, 0};
  const NodeId string_id{String{kGuid.ToString()}, 0};
  EXPECT_NE(guid_id, string_id);
  EXPECT_EQ(guid_id.type(), NodeIdType::Guid);
  EXPECT_EQ(string_id.type(), NodeIdType::String);
}

TEST(NodeIdTest, HashesGuidIdentifiers) {
  const std::hash<NodeId> hash;
  EXPECT_EQ(hash(NodeId{kGuid, 1}), hash(NodeId{kGuid, 1}));
  EXPECT_NE(hash(NodeId{kGuid, 1}), hash(NodeId{Guid{}, 1}));
}

}  // namespace
}  // namespace opcua
