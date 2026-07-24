#pragma once

#include "opcua/services/history_types.h"
#include "opcua/types/status_or.h"
#include "opcua/ua/ua_types.h"

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Bridges the generated ua:: HistoryRead/HistoryUpdate wire messages and the
// hand-written history vocabulary the service callbacks / bridge speak
// (HistoryReadRawDetails, HistoryReadEventsDetails, UpdateDataDetails, ...).
//
// Option A1: the wire variant carries ua::HistoryReadRequest /
// ua::HistoryUpdateRequest, and the raw-vs-events (and data-vs-event) split
// that opcuapp models as distinct typed requests is decoded here at the
// ServiceHandler / client boundary. The HistoryRead details ExtensionObject is
// decoded into the typed ua::ReadRawModifiedDetails / ua::ReadEventDetails, and
// the conformant ua::EventFilter (SimpleAttributeOperand select clauses + a
// ContentFilter where clause) is interpreted into the simplified domain filter
// (of_type / child_of / ACKED-UNACKED). OPC UA Part 11 §6 HistoryRead,
// https://reference.opcfoundation.org/Core/Part11/v105/docs/6

namespace opcua::history_conversion {

// A HistoryUpdate detail is data (UpdateDataDetails) or event
// (UpdateEventDetails); the wire carries an array of historyUpdateDetails
// ExtensionObjects and this server supports a single element per request.
using HistoryUpdateDetails =
    std::variant<UpdateDataDetails, UpdateEventDetails>;

// A decoded HistoryRead request: either a raw/aggregated read or an event read.
// For an event read, event_field_paths carries the select-clause browse paths
// needed to project the response events.
struct DecodedHistoryRead {
  std::variant<HistoryReadRawDetails, HistoryReadEventsDetails> details;
  std::vector<std::vector<std::string>> event_field_paths;
};

// Decodes ua::HistoryReadRequest into the managed read. Returns nullopt when
// the request is unsupported (not exactly one node, invalid TimestampsToReturn,
// an unknown details type, or a malformed / non-conformant details body) so the
// handler can answer the appropriate Bad_ status.
std::optional<DecodedHistoryRead> ToManaged(const ua::HistoryReadRequest& wire);

// Builds a single-result ua::HistoryReadResponse (service_result stays Good;
// the per-node status rides HistoryReadResult.status_code) from a managed
// result.
ua::HistoryReadResponse ToWireRawResponse(
    const StatusOr<HistoryReadRawResult>& result);
ua::HistoryReadResponse ToWireEventsResponse(
    const StatusOr<HistoryReadEventsResult>& result,
    std::span<const std::vector<std::string>> field_paths);

// Client-side request builders (the public API speaks the managed details; the
// event request selects the default BaseEventType field paths, as the server
// projects the response onto whatever the client selected).
ua::HistoryReadRequest ToWireRawRequest(const HistoryReadRawDetails& details);
ua::HistoryReadRequest ToWireEventsRequest(
    const HistoryReadEventsDetails& details);

// Client-side response decoders (events reconstruct from the default field
// paths, matching ToWireEventsRequest).
StatusOr<HistoryReadRawResult> ToManagedRawResult(
    const ua::HistoryReadResponse& wire);
StatusOr<HistoryReadEventsResult> ToManagedEventsResult(
    const ua::HistoryReadResponse& wire);

// EventFilter struct <-> managed model. Exposed for the details codec above and
// for unit testing. ToManagedEventFilter also yields the select-clause field
// paths; ToUaEventFilter emits the conformant SimpleAttributeOperand select
// clauses plus the OfType / RelatedTo / Equals(AckedState) where clause (OPC UA
// Part 4 §7.22.3), matching what foreign servers and clients expect.
ua::EventFilter ToUaEventFilter(
    std::span<const std::vector<std::string>> field_paths,
    const EventFilter& filter);
EventFilter ToManagedEventFilter(
    const ua::EventFilter& wire,
    std::vector<std::vector<std::string>>& field_paths);

// Whether a HistoryRead request/response carries an event (vs raw/processed
// data) payload, so a transport that keeps distinct service names (the
// websocket) can pick the right one. Determined by the details / history_data
// ExtensionObject type.
bool IsEventsRequest(const ua::HistoryReadRequest& wire);
bool IsEventsResponse(const ua::HistoryReadResponse& wire);

// HistoryUpdate: decode the single wire detail; build the response; and the
// client-side inverses.
std::optional<HistoryUpdateDetails> ToManaged(
    const ua::HistoryUpdateRequest& wire);
ua::HistoryUpdateResponse ToWire(const HistoryUpdateResult& result);
ua::HistoryUpdateRequest ToWire(const HistoryUpdateDetails& details);
HistoryUpdateResult ToManaged(const ua::HistoryUpdateResponse& wire);

}  // namespace opcua::history_conversion
