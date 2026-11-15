#include "opcua/events/event_filter.h"

#include <gtest/gtest.h>

#include <algorithm>

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
  event.source_node_id = NumericNode(777, 4);
  event.time = DateTime::Now();
  event.message = LocalizedText{u"alarm"};
  event.severity = 900;

  const auto fields = ProjectEventFields(
      {{"Severity"}, {"Message"}, {"EventId"}, {"UnknownField"}}, event);

  ASSERT_EQ(fields.size(), 4u);
  EXPECT_EQ(fields[0].get<UInt16>(), 900u);
  EXPECT_EQ(fields[1].get<LocalizedText>(), LocalizedText{u"alarm"});
  EXPECT_EQ(fields[2].get<ByteString>(), EncodeEventIdByteString(11));
  EXPECT_TRUE(fields[3].is_null());
}

TEST(EventFilterTest, SourceNameProjectsResolvedNameWithNodeIdFallback) {
  // SourceName is the resolved DisplayName when the producer supplied one,
  // falling back to the NodeId string. OPC UA Part 5 §6.4.2 BaseEventType,
  // https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2
  Event event;
  event.source_node_id = NumericNode(777, 4);

  const auto fallback = ProjectEventFields({{"SourceName"}}, event);
  ASSERT_EQ(fallback.size(), 1u);
  EXPECT_EQ(fallback[0].get<String>(), event.source_node_id.ToString());

  event.source_name = "Pump 4";
  const auto resolved = ProjectEventFields({{"SourceName"}}, event);
  ASSERT_EQ(resolved.size(), 1u);
  EXPECT_EQ(resolved[0].get<String>(), "Pump 4");

  const Event decoded = ReconstructEventFromFields({{"SourceName"}}, resolved);
  EXPECT_EQ(decoded.source_name, "Pump 4");
}

TEST(EventFilterTest, EventIdByteStringRoundTrips) {
  // 8-byte big-endian layout: lexicographic order equals numeric order and
  // the value round-trips exactly. OPC UA Part 5 §6.4.2 BaseEventType,
  // https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2
  const EventId event_id = 0x0123456789abcdefull;
  const ByteString bytes = EncodeEventIdByteString(event_id);
  ASSERT_EQ(bytes.size(), 8u);
  EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0x01u);
  EXPECT_EQ(static_cast<unsigned char>(bytes[7]), 0xefu);
  EXPECT_EQ(DecodeEventIdByteString(bytes), event_id);

  EXPECT_LT(EncodeEventIdByteString(1), EncodeEventIdByteString(0x100));

  EXPECT_EQ(DecodeEventIdByteString(ByteString(7)), std::nullopt);
  EXPECT_EQ(DecodeEventIdByteString(ByteString{}), std::nullopt);
}

TEST(EventFilterTest, ReconstructEventDecodesByteStringEventId) {
  const auto fields =
      ProjectEventFields(DefaultEventFieldPaths(), [] {
        Event event;
        event.event_id = 42;
        event.time = DateTime::Now();
        return event;
      }());

  const Event event =
      ReconstructEventFromFields(DefaultEventFieldPaths(), fields);
  EXPECT_EQ(event.event_id, 42u);
}

TEST(EventFilterTest, ReconstructEventAcceptsLegacyUInt64EventId) {
  // Tolerant decode for the rollout window: pre-ADR-0005 peers project
  // EventId as a raw UInt64.
  const Event event = ReconstructEventFromFields(
      {{"EventId"}}, {Variant{static_cast<UInt64>(43)}});
  EXPECT_EQ(event.event_id, 43u);
}

// A third-party server's EventId is whatever length it likes; open62541's is
// 16 bytes. It used to be dropped as malformed, which left the event with no
// id and every downstream check then threw it away (backlog 1055).
TEST(EventFilterTest, ReconstructEventKeepsAForeignEventIdByteString) {
  const ByteString foreign_id{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                              'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p'};
  const Event event =
      ReconstructEventFromFields({{"EventId"}}, {Variant{foreign_id}});
  EXPECT_EQ(event.foreign_event_id, foreign_id);
  EXPECT_EQ(event.event_id, LocalEventIdForForeignByteString(foreign_id));
  EXPECT_NE(event.event_id, 0u);
}

TEST(EventFilterTest, ReconstructEventIgnoresAnEmptyEventIdByteString) {
  const Event event =
      ReconstructEventFromFields({{"EventId"}}, {Variant{ByteString{}}});
  EXPECT_EQ(event.event_id, 0u);
  EXPECT_TRUE(event.foreign_event_id.empty());
}

TEST(EventFilterTest, LocalIdsOfDifferentForeignEventIdsDiffer) {
  EXPECT_NE(LocalEventIdForForeignByteString(ByteString{'a'}),
            LocalEventIdForForeignByteString(ByteString{'b'}));
  EXPECT_EQ(LocalEventIdForForeignByteString(ByteString{'a', 'b'}),
            LocalEventIdForForeignByteString(ByteString{'a', 'b'}));
}

// The Condition list reads a Condition's identity and AckedState/Id (OPC UA
// Part 9 §5.5.2 and §5.7.2).
TEST(EventFilterTest, ReconstructEventReadsConditionFields) {
  const auto& paths = ConditionEventFieldPaths();
  std::vector<Variant> fields(paths.size());
  fields[0] = Variant{ByteString{'x', 'y', 'z'}};
  fields[paths.size() - 2] = Variant{NodeId{7, 3}};
  fields[paths.size() - 1] = Variant{true};
  const Event event = ReconstructEventFromFields(paths, fields);
  EXPECT_EQ(event.condition_id, (NodeId{7, 3}));
  EXPECT_TRUE(event.acked);
}

// The standard lists name no SCADA extension field: a strict server rejects
// a filter that does.
TEST(EventFilterTest, StandardFieldListsNameOnlyStandardFields) {
  const auto& defaults = DefaultEventFieldPaths();
  const auto& standard = StandardEventFieldPaths();
  ASSERT_EQ(standard.size(), 8u);
  EXPECT_TRUE(std::equal(standard.begin(), standard.end(), defaults.begin()));
  EXPECT_EQ(standard.back(), std::vector<std::string>{"Severity"});

  const auto& condition = ConditionEventFieldPaths();
  ASSERT_EQ(condition.size(), 10u);
  EXPECT_EQ(condition[8],
            std::vector<std::string>{std::string{kConditionIdFieldName}});
  EXPECT_EQ(condition[9], (std::vector<std::string>{"AckedState", "Id"}));
}

TEST(EventFilterTest, KnownFieldPathsAreFoundByCount) {
  EXPECT_EQ(KnownEventFieldPathsForCount(DefaultEventFieldPaths().size()),
            &DefaultEventFieldPaths());
  EXPECT_EQ(KnownEventFieldPathsForCount(8), &StandardEventFieldPaths());
  EXPECT_EQ(KnownEventFieldPathsForCount(10), &ConditionEventFieldPaths());
  EXPECT_EQ(KnownEventFieldPathsForCount(3), nullptr);
}

}  // namespace
}  // namespace opcua
