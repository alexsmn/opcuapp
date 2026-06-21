#pragma once

#include "opcua/scada/aggregate_filter.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/event.h"
#include "opcua/scada/event_filter.h"

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
  opcua::base::Time from;
  opcua::base::Time to;
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
  opcua::base::Time from;
  opcua::base::Time to;
  EventFilter filter;
};

// Result of a raw/aggregated HistoryRead: the returned DataValues plus an
// optional continuation point. OPC UA Part 4 §5.11.3 HistoryRead,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.3
struct HistoryReadRawResult {
  Status status{StatusCode::Good};
  std::vector<DataValue> values;
  ByteString continuation_point;
};

// Result of an event HistoryRead: the returned historical events. OPC UA Part 4
// §5.11.3 HistoryRead,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.3
struct HistoryReadEventsResult {
  Status status{StatusCode::Good};
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

// Result of a HistoryUpdate operation: an operation-level status plus one
// StatusCode per supplied value. OPC UA Part 4 §5.10.5 HistoryUpdate,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.10.5
struct HistoryUpdateResult {
  Status status{StatusCode::Good};
  std::vector<StatusCode> operation_results;
};

// opcuapp callback delivering the per-event results of an event-acknowledgement
// operation (a SCADA extension; not a standard OPC UA service callback).
using AcknowledgeCallback =
    std::function<void(Status status, std::vector<StatusCode> results)>;

}  // namespace opcua
