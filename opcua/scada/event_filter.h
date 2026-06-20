#pragma once

#include "opcua/scada/node_id.h"

#include <vector>

namespace opcua {
namespace scada {

struct EventFilter {
  enum EventType { ACKED = 1 << 0, UNACKED = 1 << 1 };

  static const unsigned ALL_TYPES = 0;

  EventFilter& set_of_type(std::vector<NodeId> of_type) {
    this->of_type = std::move(of_type);
    return *this;
  }

  EventFilter& add_of_type(const NodeId& type_definition_id) {
    of_type.emplace_back(type_definition_id);
    return *this;
  }

  EventFilter& set_child_of(std::vector<NodeId> child_of) {
    this->child_of = std::move(child_of);
    return *this;
  }

  EventFilter& add_child_of(const NodeId& parent_id) {
    child_of.emplace_back(parent_id);
    return *this;
  }

  bool operator==(const EventFilter&) const = default;

  // A bit mask of `EventType`. Zero means no filter, for both real-time and
  // historical events.
  unsigned types = 0;

  std::vector<NodeId> of_type;
  std::vector<NodeId> child_of;
};

std::ostream& operator<<(std::ostream& stream, const EventFilter& event_filter);

}  // namespace scada
}  // namespace opcua (vendored)
