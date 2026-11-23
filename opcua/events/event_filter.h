#pragma once

#include "opcua/events/event.h"
#include "opcua/types/node_id.h"
#include "opcua/types/variant.h"

#include <boost/json/value.hpp>

#include <any>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opcua {

// opcuapp/SCADA-specific filter for selecting events by acknowledgement state
// and event-type membership. It is a simplified domain analogue of the standard
// OPC UA EventFilter (select/where clauses), not a wire EventFilter. OPC UA
// Part 4 §7.22.3 EventFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.3
struct EventFilter {
  // RETAINED widens UNACKED: it also selects the latest notification of every
  // Condition that is still active although acknowledged — Part 9's Retain
  // (OPC UA Part 9 §5.5.2 ConditionType,
  // https://reference.opcfoundation.org/Core/Part9/v105/docs/5.5.2). A reader
  // sends it together with UNACKED, so a server that predates it still answers
  // the unacknowledged set rather than every event it holds (backlog 1117).
  enum EventType { ACKED = 1 << 0, UNACKED = 1 << 1, RETAINED = 1 << 2 };

  static const unsigned ALL_TYPES = 0;

  bool operator==(const EventFilter&) const = default;

  // A bit mask of `EventType`. Zero means no filter, for both real-time and
  // historical events.
  unsigned types = 0;

  std::vector<NodeId> of_type;
  std::vector<NodeId> child_of;
};

std::ostream& operator<<(std::ostream& stream, const EventFilter& event_filter);

// Default BaseEventType fields used when an EventFilter omits select clauses.
// OPC UA Part 4 §7.22.3 EventFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.3
const std::vector<std::vector<std::string>>& DefaultEventFieldPaths();

// The Part 9 ConditionId has no browse path: it is selected with an empty one
// and the NodeId attribute, typed by ConditionType (OPC UA Part 9 §5.5.2
// ConditionType, Table 10,
// https://reference.opcfoundation.org/Core/Part9/v105/docs/5.5.2). A field-path
// list names it with this one pseudo segment, which the select-clause builder
// (`opcua/session/subscription_conversion.cpp`) turns into that operand.
inline constexpr std::string_view kConditionIdFieldName = "ConditionId";

// The BaseEventType fields every OPC UA server can project (OPC UA Part 5
// §6.4.2 BaseEventType,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2): the first
// eight of DefaultEventFieldPaths, without the SCADA extensions a third-party
// server has no field for. A strict server (open62541) rejects a whole event
// MonitoredItem when any select clause names a field no event type declares.
const std::vector<std::vector<std::string>>& StandardEventFieldPaths();

// StandardEventFieldPaths plus what a client needs to follow a Condition: its
// ConditionId and AckedState/Id (OPC UA Part 9 §5.7.2
// AcknowledgeableConditionType,
// https://reference.opcfoundation.org/Core/Part9/v105/docs/5.7.2). A server
// that does not know AcknowledgeableConditionType may reject the whole filter;
// the caller then falls back to StandardEventFieldPaths.
const std::vector<std::vector<std::string>>& ConditionEventFieldPaths();

// DefaultEventFieldPaths plus our own server's Condition fields: ConditionId
// and ActiveState/Id (OPC UA Part 9 §5.8.2 AlarmConditionType,
// https://reference.opcfoundation.org/Core/Part9/v105/docs/5.8.2), appended so
// the default list keeps its positions. A live subscription asks for it by
// name; a peer that names no select clauses still gets the default list, so a
// client built before backlog 1117 keeps decoding notifications by count.
// HistoryRead events use it on both ends; the decode tolerates either length.
const std::vector<std::vector<std::string>>& ScadaConditionEventFieldPaths();

// The known field-path list `count` event fields were selected with: an
// EventFieldList carries no field names, and the four lists above have four
// different lengths. Null for any other count.
const std::vector<std::vector<std::string>>* KnownEventFieldPathsForCount(
    std::size_t count);

// Returns `field_paths` unless it is empty; empty filters are normalized to the
// standard BaseEventType fields reported by this stack.
std::vector<std::vector<std::string>> NormalizeEventFieldPaths(
    std::vector<std::vector<std::string>> field_paths);

// Extracts select-clause browse paths from a JSON EventFilter representation.
std::vector<std::vector<std::string>> ParseEventFilterFieldPaths(
    const boost::json::value& raw_filter);

// Builds the JSON EventFilter representation used by websocket transport and
// by binary-codec round-tripping of decoded EventFilters.
boost::json::value BuildEventFilter(
    std::span<const std::vector<std::string>> field_paths);

// Projects an event into EventFieldList field values in select-clause order.
std::vector<Variant> ProjectEventFields(
    const std::vector<std::vector<std::string>>& field_paths,
    const std::any& event);

// Inverse of ProjectEventFields: rebuilds an Event from select-clause field
// values, mapping each `field_paths[i]` browse path back to the Event property
// it selected. Inherently partial -- only the selected (BaseEventType) fields
// are recoverable -- so unselected properties keep their defaults. Used by the
// client-side HistoryReadEvents response decode.
Event ReconstructEventFromFields(
    const std::vector<std::vector<std::string>>& field_paths,
    const std::vector<Variant>& fields);

}  // namespace opcua
