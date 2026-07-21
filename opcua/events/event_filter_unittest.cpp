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
  event.source_node_id = NumericNode(777, 4);
  event.time = DateTime::Now();
  event.message = LocalizedText{u"alarm"};
  event.severity = 900;

  const auto fields = ProjectEventFields(
      {{"Severity"}, {"Message"}, {"EventId"}, {"UnknownField"}}, event);

  ASSERT_EQ(fields.size(), 4u);
  EXPECT_EQ(fields[0].get<UInt32>(), 900u);
  EXPECT_EQ(fields[1].get<LocalizedText>(), LocalizedText{u"alarm"});
  EXPECT_EQ(fields[2].get<ByteString>(), EncodeEventIdByteString(11));
  EXPECT_TRUE(fields[3].is_null());
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

TEST(EventFilterTest, ReconstructEventIgnoresMalformedEventIdByteString) {
  const Event event =
      ReconstructEventFromFields({{"EventId"}}, {Variant{ByteString(3)}});
  EXPECT_EQ(event.event_id, 0u);
}

}  // namespace
}  // namespace opcua
