#include "opcua/types/duration.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

TEST(DurationTest, InternalValueUsesOpcUaDoubleMilliseconds) {
  const Duration duration = Duration::FromInternalValue(1.25);

  EXPECT_DOUBLE_EQ(duration.ToInternalValue(), 1.25);
  EXPECT_DOUBLE_EQ(duration.InMillisecondsF(), 1.25);
  EXPECT_EQ(duration.InMicroseconds(), 1250);
}

TEST(DurationTest, PreservesFractionalMilliseconds) {
  const Duration duration = Duration::FromInternalValue(0.0001);

  EXPECT_DOUBLE_EQ(duration.ToInternalValue(), 0.0001);
  EXPECT_EQ(duration.InNanoseconds(), 100);
  EXPECT_DOUBLE_EQ((duration * 2).ToInternalValue(), 0.0002);
}

}  // namespace
}  // namespace opcua
