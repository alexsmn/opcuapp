#pragma once

#include "opcua/events/event.h"
#include "opcua/types/node_id.h"
#include "opcua/types/variant.h"

#include <boost/json/value.hpp>

#include <any>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace opcua {

// opcuapp/SCADA-specific filter for selecting events by acknowledgement state
// and event-type membership. It is a simplified domain analogue of the standard
// OPC UA EventFilter (select/where clauses), not a wire EventFilter. OPC UA
// Part 4 §7.22.3 EventFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.3
struct EventFilter {
  enum EventType { ACKED = 1 << 0, UNACKED = 1 << 1 };

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
