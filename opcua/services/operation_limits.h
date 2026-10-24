#pragma once

#include <cstddef>
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
  std::uint32_t max_nodes_per_history_update_data = 1000;
  std::uint32_t max_monitored_items_per_call = 1000;

  // The largest ByteString, in bytes, that a Write value or a Method input
  // argument may carry. Not an OperationLimits member on the wire: it is
  // published as Server.ServerCapabilities.MaxByteStringLength — OPC UA Part 5
  // §6.3.2 ServerCapabilitiesType,
  // https://reference.opcfoundation.org/Core/Part5/v105/docs/6.3.2 — and lives
  // here because this struct is what keeps a published limit and the enforced
  // one the same value. Zero means no limit, and Part 5 then requires the
  // property not to be provided at all.
  std::uint32_t max_byte_string_length = 0;
};

// Maximum number of Browse continuation points the server keeps per session.
// Exposed as Server.ServerCapabilities.MaxBrowseContinuationPoints and enforced
// by ServerSession (a Browse that would exceed it returns
// Bad_NoContinuationPoints). OPC UA Part 4 §5.9.2,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.9.2
inline constexpr std::uint32_t kMaxBrowseContinuationPoints = 100;

// Per-session subscription quotas, for a listener exposed to untrusted peers.
// Every field defaults to "no limit", which is what every listener had before
// these existed. A limit is enforced by refusal where Part 4 defines a refusal
// code and by revision where it defines revision:
//
// - max_subscriptions_per_session: CreateSubscription answers
//   Bad_TooManySubscriptions (OPC UA Part 4 §5.14.2.3 Service results,
//   https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.2.3).
// - max_monitored_items_per_subscription / _per_session: each item past the
//   limit gets Bad_TooManyMonitoredItems (§5.13.2.4 StatusCodes,
//   https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2.4).
// - min_publishing_interval: the requested publishingInterval is revised up to
//   it (§5.14.2 CreateSubscription, §5.14.3 ModifySubscription).
// - min_sampling_interval / max_queue_size: the requested samplingInterval is
//   revised up and the queueSize down (§5.13.2 CreateMonitoredItems,
//   https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.2).
// - max_publish_requests_per_session: past it the OLDEST queued Publish is
//   answered Bad_TooManyPublishRequests (§5.14.5.1 Description,
//   https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5.1).
struct SubscriptionLimits {
  std::size_t max_subscriptions_per_session = 0;
  std::size_t max_monitored_items_per_subscription = 0;
  std::size_t max_monitored_items_per_session = 0;
  double min_publishing_interval_ms = 0;
  double min_sampling_interval_ms = 0;
  std::uint32_t max_queue_size = 0;
  std::size_t max_publish_requests_per_session = 0;
};

}  // namespace opcua
