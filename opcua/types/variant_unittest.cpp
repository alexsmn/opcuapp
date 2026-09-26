#include "opcua/types/variant.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

// opcuapp carries no operator-facing wording, so a bool renders as an
// invariant word in every string form rather than in any one language.
TEST(VariantTest, BoolRendersAsInvariantText) {
  EXPECT_EQ(ToString16(Variant{true}), u"true");
  EXPECT_EQ(ToString16(Variant{false}), u"false");

  LocalizedText text;
  ASSERT_TRUE(Variant{true}.get(text));
  EXPECT_EQ(ToString16(text), u"true");
  ASSERT_TRUE(Variant{false}.get(text));
  EXPECT_EQ(ToString16(text), u"false");
}

// The narrow form was already invariant; pinned so the two cannot drift into
// disagreeing about what a bool is.
TEST(VariantTest, BoolNarrowFormIsNumeric) {
  EXPECT_EQ(ToString(Variant{true}), "1");
  EXPECT_EQ(ToString(Variant{false}), "0");
}

}  // namespace
}  // namespace opcua
