#include "opcua/types/qualifier.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

TEST(QualifierTest, ToStringSpellsEachFlagAsALetter) {
  EXPECT_EQ(ToString(Qualifier{}), "");
  EXPECT_EQ(ToString(Qualifier{Qualifier::BAD | Qualifier::OFFLINE |
                               Qualifier::STALE}),
            "BOT");
}

// The spelled-out form is invariant: opcuapp carries no operator-facing
// wording, so each flag is its enum name and an application words it.
TEST(QualifierTest, ToString16SpellsEachFlagAsItsEnumName) {
  EXPECT_EQ(ToString16(Qualifier{}), u"");
  EXPECT_EQ(ToString16(Qualifier{Qualifier::BAD}), u"BAD ");
  EXPECT_EQ(
      ToString16(Qualifier{
          Qualifier::BAD | Qualifier::BACKUP | Qualifier::OFFLINE |
          Qualifier::MANUAL | Qualifier::MISCONFIGURED | Qualifier::SIMULATED |
          Qualifier::SPORADIC | Qualifier::STALE | Qualifier::FAILED}),
      u"BAD BACKUP OFFLINE MANUAL MISCONFIGURED SIMULATED SPORADIC STALE "
      u"FAILED ");
}

}  // namespace
}  // namespace opcua
