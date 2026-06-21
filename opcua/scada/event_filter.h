#pragma once

#include "opcua/scada/node_id.h"

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

}  // namespace opcua (vendored)
