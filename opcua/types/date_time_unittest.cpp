#include "opcua/types/date_time.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

TEST(DateTimeTest, InternalValueUsesOpcUaDateTimeTicks) {
  EXPECT_EQ(DateTime{}.ToInternalValue(), 0);
  EXPECT_EQ(DateTime::UnixEpoch().ToInternalValue(),
            DateTime::kTimeTToDateTimeTicksOffset);

  constexpr int64_t kRawValue = 1234567;
  const DateTime time = DateTime::FromInternalValue(kRawValue);

  EXPECT_EQ(time.ToInternalValue(), kRawValue);
  EXPECT_EQ(time.ToDeltaSinceWindowsEpoch().InMicroseconds(),
            kRawValue / DateTime::kTicksPerMicrosecond);
}

TEST(DateTimeTest, DurationArithmeticScalesMicrosecondsToDateTimeTicks) {
  constexpr int64_t kRawValue = DateTime::kTimeTToDateTimeTicksOffset + 7;
  const DateTime time = DateTime::FromInternalValue(kRawValue);

  EXPECT_EQ((time + Duration::FromMicroseconds(1)).ToInternalValue(),
            kRawValue + DateTime::kTicksPerMicrosecond);
  EXPECT_EQ((time - Duration::FromMicroseconds(1)).ToInternalValue(),
            kRawValue - DateTime::kTicksPerMicrosecond);
}

}  // namespace
}  // namespace opcua
