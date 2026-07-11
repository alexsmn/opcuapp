#include "opcua/base/bit_mask_string.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace opcua {
namespace {

constexpr std::array<std::string_view, 3> kLabels{"Select", "Parameter",
                                                  "Test"};

TEST(OpcuaBitMaskToStringTest, NoBitsSetIsEmptyArray) {
  EXPECT_EQ(BitMaskToString(0, kLabels), "[]");
}

TEST(OpcuaBitMaskToStringTest, SingleBit) {
  EXPECT_EQ(BitMaskToString(0b001, kLabels), R"(["Select"])");
  EXPECT_EQ(BitMaskToString(0b100, kLabels), R"(["Test"])");
}

TEST(OpcuaBitMaskToStringTest, MultipleBitsAreCommaSeparated) {
  EXPECT_EQ(BitMaskToString(0b101, kLabels), R"(["Select","Test"])");
}

TEST(OpcuaBitMaskToStringTest, BitsWithoutLabelsAreIgnored) {
  EXPECT_EQ(BitMaskToString(0b1001, kLabels), R"(["Select"])");
}

TEST(OpcuaBitMaskToStringTest, HighBitDoesNotOverflow) {
  // The pre-fix `1 << i` was undefined behavior for i >= 31. With 40 labels the
  // loop must cap at the width of `unsigned` rather than shift past it.
  std::array<std::string_view, 40> labels;
  for (std::string_view& label : labels)
    label = "b";
  const unsigned mask = 1u << 31;
  EXPECT_EQ(BitMaskToString(mask, labels), R"(["b"])");
  const std::string all = BitMaskToString(0xFFFFFFFFu, labels);
  EXPECT_EQ(std::count(all.begin(), all.end(), '"'), 64);  // 32 quoted labels
}

}  // namespace
}  // namespace opcua
