#include "opcua/events/event_filter.h"

#include <gtest/gtest.h>

namespace opcua {
namespace {

NodeId NumericNode(NumericId id, NamespaceIndex ns = 2) {
  return {id, ns};
}

TEST(EventFilterTest, NormalizeEventFieldPathsUsesDefaultsOnlyWhenEmpty) {
  const auto defaults = NormalizeEventFieldPaths({});
  EXPECT_EQ(defaults, DefaultEventFieldPaths());

  const std::vector<std::vector<std::string>> custom{
      {"Severity"},
      {"Message"},
  };
  EXPECT_EQ(NormalizeEventFieldPaths(custom), custom);
}

TEST(EventFilterTest, ParseAndBuildEventFilterRoundTripsFieldPaths) {
  const auto filter = BuildEventFilter(std::vector<std::vector<std::string>>{
      {"Severity"},
      {"Message"},
  });
  EXPECT_EQ(ParseEventFilterFieldPaths(filter),
            (std::vector<std::vector<std::string>>{{"Severity"}, {"Message"}}));
}

TEST(EventFilterTest, ProjectEventFieldsPreservesSelectClauseOrder) {
  Event event;
  event.event_id = 11;
  event.event_type_id = NumericNode(501, 0);
  event.node_id = NumericNode(777, 4);
  event.time = DateTime::Now();
  event.message = LocalizedText{u"alarm"};
  event.severity = 900;

  const auto fields = ProjectEventFields(
      {{"Severity"}, {"Message"}, {"EventId"}, {"UnknownField"}}, event);

  ASSERT_EQ(fields.size(), 4u);
  EXPECT_EQ(fields[0].get<UInt32>(), 900u);
  EXPECT_EQ(fields[1].get<LocalizedText>(), LocalizedText{u"alarm"});
  EXPECT_EQ(fields[2].get<UInt64>(), 11u);
  EXPECT_TRUE(fields[3].is_null());
}

}  // namespace
}  // namespace opcua
