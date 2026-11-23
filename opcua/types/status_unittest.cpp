#include "opcua/types/status.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

// The reserved severity (binary 11) is Bad to every client: "All Clients should
// treat a StatusCode with this severity as Bad" (OPC UA Part 4 §7.38.1
// General, Table 176 StatusCode bit assignments,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38.1). It used to
// read as neither good nor bad, so a peer's reserved-severity status converted
// to `true` — success.
TEST(StatusTest, ReservedSeverityIsBad) {
  const Status reserved = Status::FromFullCode(0xC0000000u);
  EXPECT_EQ(reserved.severity(), StatusSeverity::Reserved);
  EXPECT_TRUE(reserved.bad());
  EXPECT_FALSE(static_cast<bool>(reserved));
  EXPECT_TRUE(IsBad(static_cast<StatusCode>(0xC000u)));
}

// A code with bits above the 16 it holds still has its bits 14-15 as its
// severity, never a fifth value.
TEST(StatusTest, SeverityIgnoresBitsAboveTheCode) {
  const auto code = static_cast<StatusCode>(0x18000u);
  EXPECT_EQ(GetSeverity(code), StatusSeverity::Bad);
  EXPECT_TRUE(IsBad(code));
  EXPECT_FALSE(IsGood(code));
}

}  // namespace
}  // namespace opcua
