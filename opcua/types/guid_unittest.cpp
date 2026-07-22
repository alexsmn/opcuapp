#include "opcua/types/guid.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

// The example Guid from OPC UA Part 6 §5.1.3,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.3
constexpr Guid kExample{
    .data1 = 0x72962B91,
    .data2 = 0xFA75,
    .data3 = 0x4AE6,
    .data4 = {0x8D, 0x28, 0xB4, 0x04, 0xDC, 0x7D, 0xAF, 0x63}};
constexpr std::string_view kExampleText =
    "72962B91-FA75-4AE6-8D28-B404DC7DAF63";

TEST(GuidTest, FormatsCanonicalTextForm) {
  EXPECT_EQ(kExample.ToString(), kExampleText);
  EXPECT_EQ(Guid{}.ToString(), "00000000-0000-0000-0000-000000000000");
}

TEST(GuidTest, ParsesCanonicalTextForm) {
  EXPECT_EQ(Guid::FromString(kExampleText), kExample);
}

TEST(GuidTest, ParsesLowerCaseAndRoundTripsToUpperCase) {
  const std::optional<Guid> parsed =
      Guid::FromString("72962b91-fa75-4ae6-8d28-b404dc7daf63");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, kExample);
  EXPECT_EQ(parsed->ToString(), kExampleText);
}

TEST(GuidTest, RejectsMalformedText) {
  // Wrong length, missing hyphens, brace-wrapped form, and a non-hex digit.
  EXPECT_FALSE(Guid::FromString("").has_value());
  EXPECT_FALSE(
      Guid::FromString("72962B91FA754AE68D28B404DC7DAF63").has_value());
  EXPECT_FALSE(
      Guid::FromString("{72962B91-FA75-4AE6-8D28-B404DC7DAF63}").has_value());
  EXPECT_FALSE(
      Guid::FromString("72962B9Z-FA75-4AE6-8D28-B404DC7DAF63").has_value());
  EXPECT_FALSE(
      Guid::FromString("72962B91:FA75-4AE6-8D28-B404DC7DAF63").has_value());
}

TEST(GuidTest, NullGuidIsDetected) {
  EXPECT_TRUE(Guid{}.is_null());
  EXPECT_FALSE(kExample.is_null());
}

TEST(GuidTest, OrdersByFieldsInDeclarationOrder) {
  constexpr Guid kSmall{.data1 = 1};
  constexpr Guid kLarge{.data1 = 2};
  EXPECT_LT(kSmall, kLarge);
  EXPECT_NE(kSmall, kLarge);
}

}  // namespace
}  // namespace opcua
