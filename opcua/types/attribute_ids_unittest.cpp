#include "opcua/types/attribute_ids.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

// Attribute ids arrive from clients, and OPC UA defines ids past the end of
// this name table (RolePermissions through AccessLevelEx, 24-27); naming one
// used to read out of bounds.
TEST(AttributeIdsTest, AnUnknownIdHasAName) {
  EXPECT_EQ(ToString(static_cast<AttributeId>(27)), "AttributeId(27)");
  EXPECT_EQ(ToString(AttributeId::Value), "Value");
}

}  // namespace
}  // namespace opcua
