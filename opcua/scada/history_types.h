#pragma once

#include "opcua/scada/aggregate_filter.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/event.h"
#include "opcua/scada/event_filter.h"

#include <functional>
#include <memory>
#include <vector>

namespace opcua {
namespace scada {

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

struct HistoryReadEventsDetails {
  // Defines the root source node.
  NodeId node_id;
  opcua::base::Time from;
  opcua::base::Time to;
  EventFilter filter;
};

struct HistoryReadRawResult {
  Status status{StatusCode::Good};
  std::vector<DataValue> values;
  ByteString continuation_point;
};

struct HistoryReadEventsResult {
  Status status{StatusCode::Good};
  std::vector<Event> events;
};

using AcknowledgeCallback =
    std::function<void(Status status, std::vector<StatusCode> results)>;

}  // namespace scada
}  // namespace opcua (vendored)
