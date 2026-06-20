#include "opcua/namespace_table.h"

#include "opcua/scada/variant.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace opcua {
namespace {

const std::vector<std::string> kUris = {
    "http://opcfoundation.org/UA/",      // index 0 (standard)
    "urn:server:local",                  // index 1
    "http://telecontrol.ru/opcua/scada"  // index 2
};

TEST(NamespaceTableTest, FromVariantParsesStringArray) {
  const NamespaceTable table =
      NamespaceTable::FromVariant(scada::Variant{kUris});

  ASSERT_EQ(table.size(), 3u);
  EXPECT_EQ(table.uris(), kUris);
  EXPECT_FALSE(table.empty());
}

TEST(NamespaceTableTest, IndexForUriFindsKnownNamespace) {
  const NamespaceTable table{kUris};

  EXPECT_EQ(table.IndexForUri("http://opcfoundation.org/UA/"), 0);
  EXPECT_EQ(table.IndexForUri("http://telecontrol.ru/opcua/scada"), 2);
}

TEST(NamespaceTableTest, IndexForUriReturnsNulloptForUnknownNamespace) {
  const NamespaceTable table{kUris};

  EXPECT_FALSE(table.IndexForUri("urn:not:present").has_value());
}

TEST(NamespaceTableTest, UriForIndexReturnsRegisteredUri) {
  const NamespaceTable table{kUris};

  EXPECT_EQ(table.UriForIndex(1), "urn:server:local");
}

TEST(NamespaceTableTest, UriForIndexReturnsEmptyWhenOutOfRange) {
  const NamespaceTable table{kUris};

  EXPECT_TRUE(table.UriForIndex(9).empty());
}

TEST(NamespaceTableTest, FromVariantRejectsNonArrayVariant) {
  const NamespaceTable table =
      NamespaceTable::FromVariant(scada::Variant{std::string{"scalar"}});

  EXPECT_TRUE(table.empty());
}

TEST(NamespaceTableTest, FromVariantRejectsNonStringArray) {
  const NamespaceTable table = NamespaceTable::FromVariant(
      scada::Variant{std::vector<std::int32_t>{1, 2, 3}});

  EXPECT_TRUE(table.empty());
}

}  // namespace
}  // namespace opcua
