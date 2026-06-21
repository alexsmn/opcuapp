#pragma once

#include <cstdint>

namespace opcua {

// Per-server OPC UA operation limits. These are both exposed in the address
// space under Server.ServerCapabilities.OperationLimits and enforced on the
// request path: a service request whose operation array exceeds the relevant
// limit is rejected with Bad_TooManyOperations (or Bad_TooManyMonitoredItems).
// The same struct instance must drive both exposure and enforcement so the two
// never disagree (OPC UA Part 4 §5.10).
struct OperationLimits {
  std::uint32_t max_nodes_per_read = 1000;
  std::uint32_t max_nodes_per_write = 1000;
  std::uint32_t max_nodes_per_method_call = 1000;
  std::uint32_t max_nodes_per_browse = 1000;
  std::uint32_t max_nodes_per_register_nodes = 1000;
  std::uint32_t max_nodes_per_translate_browse_paths_to_node_ids = 1000;
  std::uint32_t max_nodes_per_node_management = 1000;
  std::uint32_t max_nodes_per_history_read_data = 1000;
  std::uint32_t max_nodes_per_history_read_events = 1000;
  std::uint32_t max_monitored_items_per_call = 1000;
};

// Maximum number of Browse continuation points the server keeps per session.
// Exposed as Server.ServerCapabilities.MaxBrowseContinuationPoints and enforced
// by ServerSession (a Browse that would exceed it returns
// Bad_NoContinuationPoints). OPC UA Part 4 §5.8.2,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.2
inline constexpr std::uint32_t kMaxBrowseContinuationPoints = 100;

}  // namespace opcua
