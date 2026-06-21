#pragma once

#include "opcua/scada/date_time.h"
#include "opcua/scada/node_id.h"

#include <concepts>
#include <ostream>

namespace opcua {

// Computes Aggregate values (start time, processing interval and aggregate
// type) over the values of a MonitoredItem. OPC UA Part 4 §7.22.4
// AggregateFilter,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.22.4
struct AggregateFilter {
  bool is_null() const { return interval.is_zero(); }

  std::strong_ordering operator<=>(const AggregateFilter&) const = default;

  DateTime start_time;
  Duration interval;
  NodeId aggregate_type;
};

static_assert(std::totally_ordered<AggregateFilter>);

std::ostream& operator<<(std::ostream& stream, const AggregateFilter& filter);

}  // namespace opcua (vendored)
