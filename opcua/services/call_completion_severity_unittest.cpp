#include "opcua/services/call_completion_severity.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

// The whole point of the rule is that a healthy steady state is silent, so this
// is the case that keeps the log volume down.
TEST(CallCompletionSeverityTest, PromptSuccessIsDebug) {
  EXPECT_EQ(CallCompletionSeverity(Status{}, Duration::FromMilliseconds(0)),
            BoostLogSeverity::debug);
  EXPECT_EQ(CallCompletionSeverity(Status{}, Duration::FromMilliseconds(56)),
            BoostLogSeverity::debug);
}

// ...and the other half of the point: a stall must still reach a stdout-only
// collector, which is how the two production incidents in the header were seen.
TEST(CallCompletionSeverityTest, SlowSuccessIsInfo) {
  EXPECT_EQ(CallCompletionSeverity(Status{}, Duration::FromMilliseconds(25669)),
            BoostLogSeverity::info);
}

TEST(CallCompletionSeverityTest, FailureIsInfoHoweverFast) {
  EXPECT_EQ(CallCompletionSeverity(Status{StatusCode::Bad},
                                   Duration::FromMilliseconds(0)),
            BoostLogSeverity::info);
}

// The boundary is inclusive: exactly at the threshold is already "slow". Pinned
// because the comparison is the only thing separating a silent steady state
// from a noisy one, and an off-by-one here is invisible in production.
TEST(CallCompletionSeverityTest, ThresholdIsInclusive) {
  EXPECT_EQ(CallCompletionSeverity(Status{}, kSlowCallLogThreshold),
            BoostLogSeverity::info);
  EXPECT_EQ(CallCompletionSeverity(Status{}, kSlowCallLogThreshold -
                                                 Duration::FromMicroseconds(1)),
            BoostLogSeverity::debug);
}

}  // namespace
}  // namespace opcua
