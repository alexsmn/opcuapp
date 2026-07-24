#pragma once

#include "opcua/events/aggregate_filter.h"
#include "opcua/events/event.h"
#include "opcua/events/event_filter.h"
#include "opcua/types/data_value.h"

#include <functional>
#include <memory>
#include <vector>

namespace opcua {

// Selects historical raw (or aggregated) values for a node over a time range
// for HistoryRead, with optional continuation. Corresponds to
// ReadRawModifiedDetails / ReadProcessedDetails. OPC UA Part 11 §6 HistoryRead
// (HistoryReadDetails); exact subsection not verified, parent section cited,
// https://reference.opcfoundation.org/Core/Part11/v105/docs/6
struct HistoryReadRawDetails {
  bool forward() const { return to.is_null() || from <= to; }

  NodeId node_id;
  opcua::DateTime from;
  opcua::DateTime to;
  size_t max_count = 0;
  AggregateFilter aggregation;
  bool release_continuation_point = false;
  ByteString continuation_point;
};

// Selects historical events for a node over a time range for HistoryRead.
// Corresponds to ReadEventDetails. OPC UA Part 11 §6 HistoryRead
// (HistoryReadDetails); exact subsection not verified, parent section cited,
// https://reference.opcfoundation.org/Core/Part11/v105/docs/6
struct HistoryReadEventsDetails {
  // Defines the root source node.
  NodeId node_id;
  opcua::DateTime from;
  opcua::DateTime to;
  EventFilter filter;
};

// Result of a raw/aggregated HistoryRead: the returned DataValues plus an
// optional continuation point. OPC UA Part 4 §5.11.3 HistoryRead,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.3
// Operation-level failure is reported by the enclosing `StatusOr`, not by a
// field here.
struct HistoryReadRawResult {
  std::vector<DataValue> values;
  ByteString continuation_point;
};

// Result of an event HistoryRead: the returned historical events. OPC UA Part 4
// §5.11.3 HistoryRead,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.3
// Operation-level failure is reported by the enclosing `StatusOr`, not by a
// field here.
struct HistoryReadEventsResult {
  std::vector<Event> events;
};

// How a HistoryUpdate applies the supplied values. OPC UA Part 11 §6.8.3
// PerformUpdateType,
// https://reference.opcfoundation.org/Core/Part11/v105/docs/6.8.3
enum class PerformUpdateType {
  Insert = 1,
  Replace = 2,
  Update = 3,
  Remove = 4,
};

// Insert/replace historical data values for a node. OPC UA Part 11 §6.8.2
// UpdateDataDetails,
// https://reference.opcfoundation.org/Core/Part11/v105/docs/6.8.2
struct UpdateDataDetails {
  NodeId node_id;
  PerformUpdateType perform_insert_replace = PerformUpdateType::Update;
  std::vector<DataValue> values;
};

// Insert historical events for a node. OPC UA Part 11 §6.8.4
// UpdateEventDetails,
// https://reference.opcfoundation.org/Core/Part11/v105/docs/6.8.4
//
// Events cross the wire projected onto the default BaseEventType select clauses
// (see DefaultEventFieldPaths), exactly as the event HistoryRead response does;
// only those fields round-trip. The structured Event is reconstructed on
// receipt.
struct UpdateEventDetails {
  NodeId node_id;
  PerformUpdateType perform_insert_replace = PerformUpdateType::Insert;
  std::vector<Event> events;
};

// opcuapp callback delivering the per-event results of an event-acknowledgement
// operation (a SCADA extension; not a standard OPC UA service callback).
using AcknowledgeCallback =
    std::function<void(Status status, std::vector<StatusCode> results)>;

}  // namespace opcua
