#include "opcua/transport/binary/egress_meter.h"

#include <gtest/gtest.h>

namespace opcua::binary {
namespace {

class EgressMeterTest : public ::testing::Test {
 protected:
  std::function<DateTime()> Clock() {
    return [this] { return now_; };
  }

  // 2026-10-24 12:00 UTC.
  DateTime now_ = DateTime::UnixEpoch() + Duration::FromDays(20750) +
                  Duration::FromHours(12);
};

TEST_F(EgressMeterTest, BudgetRunsOutAndRollsOverAtMidnightUtc) {
  EgressBudget budget{1000, Clock()};
  budget.Record(600);
  EXPECT_FALSE(budget.exhausted());
  budget.Record(400);
  EXPECT_TRUE(budget.exhausted());

  now_ = now_ + Duration::FromHours(11) + Duration::FromMinutes(59);
  EXPECT_TRUE(budget.exhausted()) << "still the same UTC day";

  now_ = now_ + Duration::FromMinutes(1);
  EXPECT_FALSE(budget.exhausted());
  EXPECT_EQ(budget.sent_today(), 0u);
  EXPECT_EQ(budget.sent_total(), 1000u);
}

TEST_F(EgressMeterTest, ZeroBudgetNeverRunsOut) {
  EgressBudget budget{0, Clock()};
  budget.Record(1'000'000'000);
  EXPECT_FALSE(budget.exhausted());
  EXPECT_EQ(budget.sent_total(), 1'000'000'000u);
}

TEST_F(EgressMeterTest, PacerAllowsOneSecondBurstThenDelays) {
  ConnectionPacer pacer{1000, Clock()};
  EXPECT_EQ(pacer.DelayFor(600), Duration{});
  EXPECT_EQ(pacer.DelayFor(400), Duration{});
  // The bucket is empty: 500 more bytes owe half a second.
  EXPECT_EQ(pacer.DelayFor(500), Duration::FromMilliseconds(500));
}

TEST_F(EgressMeterTest, PacerRefillsWithTime) {
  ConnectionPacer pacer{1000, Clock()};
  EXPECT_EQ(pacer.DelayFor(1000), Duration{});
  now_ = now_ + Duration::FromMilliseconds(300);
  EXPECT_EQ(pacer.DelayFor(300), Duration{});
  EXPECT_EQ(pacer.DelayFor(100), Duration::FromMilliseconds(100));
  // Refill never exceeds the one-second burst.
  now_ = now_ + Duration::FromSeconds(60);
  EXPECT_EQ(pacer.DelayFor(1000), Duration{});
  EXPECT_GT(pacer.DelayFor(1), Duration{});
}

TEST_F(EgressMeterTest, ZeroRateNeverDelays) {
  ConnectionPacer pacer{0, Clock()};
  EXPECT_EQ(pacer.DelayFor(100'000'000), Duration{});
}

}  // namespace
}  // namespace opcua::binary
