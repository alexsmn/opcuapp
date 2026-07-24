#include "opcua/services/history_conversion.h"

#include "opcua/events/event_filter.h"
#include "opcua/types/attribute_ids.h"
#include "opcua/types/duration.h"
#include "opcua/types/standard_node_ids.h"
#include "opcua/ua/ua_binary_codec.h"

#include <any>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace opcua::history_conversion {
namespace {

constexpr std::uint32_t kValueAttribute =
    static_cast<std::uint32_t>(AttributeId::Value);

// The domain and generated PerformUpdateType are distinct enums sharing their
// integer values (OPC UA Part 11 §6.8.3); bridge by underlying value.
template <class To, class From>
To CastEnum(From value) {
  return static_cast<To>(static_cast<std::underlying_type_t<From>>(value));
}

// A conformant select clause selecting `browse_path` under BaseEventType.Value,
// matching the hand-written AppendSimpleAttributeOperand.
ua::SimpleAttributeOperand MakeSelectClause(
    std::span<const std::string> browse_path) {
  ua::SimpleAttributeOperand clause;
  clause.type_definition_id = NodeId{id::BaseEventType};
  clause.browse_path.reserve(browse_path.size());
  for (const auto& segment : browse_path)
    clause.browse_path.push_back(QualifiedName{segment, 0});
  clause.attribute_id = kValueAttribute;
  return clause;
}

ExtensionObject MakeLiteralOperand(Variant value) {
  ua::LiteralOperand literal;
  literal.value = std::move(value);
  return ua::ToExtensionObject(literal);
}

}  // namespace

EventFilter ToManagedEventFilter(
    const ua::EventFilter& wire,
    std::vector<std::vector<std::string>>& field_paths) {
  field_paths.clear();
  field_paths.reserve(wire.select_clauses.size());
  for (const auto& clause : wire.select_clauses) {
    std::vector<std::string> path;
    path.reserve(clause.browse_path.size());
    for (const auto& segment : clause.browse_path)
      path.push_back(segment.name());
    field_paths.push_back(std::move(path));
  }

  EventFilter filter;
  for (const auto& element : wire.where_clause.elements) {
    const auto& operands = element.filter_operands;
    if (element.filter_operator == ua::FilterOperator::OfType &&
        operands.size() == 1) {
      ua::LiteralOperand literal;
      if (ua::FromExtensionObject(operands[0], literal) &&
          literal.value.type() == Variant::NODE_ID) {
        filter.of_type.push_back(literal.value.as_node_id());
      }
    } else if (element.filter_operator == ua::FilterOperator::RelatedTo &&
               operands.size() == 1) {
      ua::LiteralOperand literal;
      if (ua::FromExtensionObject(operands[0], literal) &&
          literal.value.type() == Variant::NODE_ID) {
        filter.child_of.push_back(literal.value.as_node_id());
      }
    } else if (element.filter_operator == ua::FilterOperator::Equals &&
               operands.size() == 2) {
      // `Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean))`, in
      // either operand order, maps onto the ACKED/UNACKED selection.
      for (int i = 0; i < 2; ++i) {
        ua::SimpleAttributeOperand attribute;
        ua::LiteralOperand literal;
        if (ua::FromExtensionObject(operands[i], attribute) &&
            !attribute.browse_path.empty() &&
            attribute.browse_path.back().name() == "AckedState" &&
            ua::FromExtensionObject(operands[1 - i], literal) &&
            literal.value.type() == Variant::BOOL) {
          filter.types |= literal.value.as_bool() ? EventFilter::ACKED
                                                  : EventFilter::UNACKED;
          break;
        }
      }
    }
    // else: unsupported operator/shape -- ignored (best-effort selection).
  }
  return filter;
}

ua::EventFilter ToUaEventFilter(
    std::span<const std::vector<std::string>> field_paths,
    const EventFilter& filter) {
  ua::EventFilter wire;
  wire.select_clauses.reserve(field_paths.size());
  for (const auto& path : field_paths)
    wire.select_clauses.push_back(MakeSelectClause(path));

  for (const auto& of_type : filter.of_type) {
    ua::ContentFilterElement element;
    element.filter_operator = ua::FilterOperator::OfType;
    element.filter_operands.push_back(MakeLiteralOperand(Variant{of_type}));
    wire.where_clause.elements.push_back(std::move(element));
  }
  for (const auto& child_of : filter.child_of) {
    ua::ContentFilterElement element;
    element.filter_operator = ua::FilterOperator::RelatedTo;
    element.filter_operands.push_back(MakeLiteralOperand(Variant{child_of}));
    wire.where_clause.elements.push_back(std::move(element));
  }
  // The bespoke ACKED/UNACKED bits travel as the standard where clause
  // `Equals(SimpleAttributeOperand("AckedState"), Literal(Boolean))` (OPC UA
  // Part 4 §7.7.3) so foreign servers see a conformant filter.
  const auto append_acked = [&](bool acked) {
    ua::ContentFilterElement element;
    element.filter_operator = ua::FilterOperator::Equals;
    element.filter_operands.push_back(ua::ToExtensionObject(
        MakeSelectClause(std::vector<std::string>{"AckedState"})));
    element.filter_operands.push_back(MakeLiteralOperand(Variant{acked}));
    wire.where_clause.elements.push_back(std::move(element));
  };
  if (filter.types & EventFilter::ACKED)
    append_acked(true);
  if (filter.types & EventFilter::UNACKED)
    append_acked(false);
  return wire;
}

std::optional<DecodedHistoryRead> ToManaged(
    const ua::HistoryReadRequest& wire) {
  // This server's HistoryRead supports exactly one node per request.
  if (wire.nodes_to_read.size() != 1)
    return std::nullopt;
  const auto timestamps = static_cast<std::uint32_t>(wire.timestamps_to_return);
  if (timestamps > static_cast<std::uint32_t>(ua::TimestampsToReturn::Neither))
    return std::nullopt;
  const auto& node = wire.nodes_to_read[0];
  // IndexRange / DataEncoding are not modelled and must be empty.
  if (!node.index_range.empty() || !node.data_encoding.name().empty() ||
      node.data_encoding.namespace_index() != 0)
    return std::nullopt;

  ua::ReadRawModifiedDetails raw;
  if (ua::FromExtensionObject(wire.history_read_details, raw)) {
    // Modified reads and bound returns are unsupported.
    if (raw.is_read_modified || raw.return_bounds)
      return std::nullopt;
    HistoryReadRawDetails details;
    details.node_id = node.node_id;
    details.from = raw.start_time;
    details.to = raw.end_time;
    details.max_count = raw.num_values_per_node;
    details.release_continuation_point = wire.release_continuation_points;
    details.continuation_point = node.continuation_point;
    return DecodedHistoryRead{.details = std::move(details)};
  }

  // Aggregated (processed) read: the aggregate rides HistoryReadRawDetails'
  // AggregateFilter. OPC UA Part 11 §6.5 ReadProcessedDetails.
  ua::ReadProcessedDetails processed;
  if (ua::FromExtensionObject(wire.history_read_details, processed)) {
    HistoryReadRawDetails details;
    details.node_id = node.node_id;
    details.from = processed.start_time;
    details.to = processed.end_time;
    details.release_continuation_point = wire.release_continuation_points;
    details.continuation_point = node.continuation_point;
    details.aggregation.start_time = processed.start_time;
    details.aggregation.interval =
        Duration::FromMillisecondsD(processed.processing_interval);
    if (!processed.aggregate_type.empty())
      details.aggregation.aggregate_type = processed.aggregate_type.front();
    return DecodedHistoryRead{.details = std::move(details)};
  }

  ua::ReadEventDetails events;
  if (ua::FromExtensionObject(wire.history_read_details, events)) {
    if (wire.release_continuation_points || !node.continuation_point.empty())
      return std::nullopt;
    HistoryReadEventsDetails details;
    details.node_id = node.node_id;
    details.from = events.start_time;
    details.to = events.end_time;
    std::vector<std::vector<std::string>> field_paths;
    details.filter = ToManagedEventFilter(events.filter, field_paths);
    field_paths = NormalizeEventFieldPaths(std::move(field_paths));
    return DecodedHistoryRead{.details = std::move(details),
                              .event_field_paths = std::move(field_paths)};
  }

  return std::nullopt;
}

ua::HistoryReadResponse ToWireRawResponse(
    const StatusOr<HistoryReadRawResult>& result) {
  ua::HistoryReadResult wire_result;
  wire_result.status_code = result.status();
  if (result.ok()) {
    ua::HistoryData data;
    data.data_values = result->values;
    wire_result.continuation_point = result->continuation_point;
    wire_result.history_data = ua::ToExtensionObject(data);
  }
  ua::HistoryReadResponse response;
  response.results.push_back(std::move(wire_result));
  return response;
}

ua::HistoryReadResponse ToWireEventsResponse(
    const HistoryReadEventsResult& result,
    std::span<const std::vector<std::string>> field_paths) {
  const auto paths =
      NormalizeEventFieldPaths(std::vector<std::vector<std::string>>(
          field_paths.begin(), field_paths.end()));
  ua::HistoryEvent history_event;
  history_event.events.reserve(result.events.size());
  for (const auto& event : result.events) {
    ua::HistoryEventFieldList list;
    list.event_fields = ProjectEventFields(paths, std::any{event});
    history_event.events.push_back(std::move(list));
  }
  ua::HistoryReadResult wire_result;
  wire_result.status_code = result.status;
  wire_result.history_data = ua::ToExtensionObject(history_event);
  ua::HistoryReadResponse response;
  response.results.push_back(std::move(wire_result));
  return response;
}

ua::HistoryReadRequest ToWireRawRequest(const HistoryReadRawDetails& details) {
  ua::HistoryReadRequest request;
  if (details.aggregation.is_null()) {
    ua::ReadRawModifiedDetails raw;
    raw.is_read_modified = false;
    raw.start_time = details.from;
    raw.end_time = details.to;
    raw.num_values_per_node = static_cast<std::uint32_t>(details.max_count);
    raw.return_bounds = false;
    request.history_read_details = ua::ToExtensionObject(raw);
  } else {
    ua::ReadProcessedDetails processed;
    processed.start_time = details.from;
    processed.end_time = details.to;
    processed.processing_interval =
        details.aggregation.interval.InMillisecondsF();
    processed.aggregate_type.push_back(details.aggregation.aggregate_type);
    request.history_read_details = ua::ToExtensionObject(processed);
  }
  request.timestamps_to_return = ua::TimestampsToReturn::Both;
  request.release_continuation_points = details.release_continuation_point;
  ua::HistoryReadValueId node;
  node.node_id = details.node_id;
  node.continuation_point = details.continuation_point;
  request.nodes_to_read.push_back(std::move(node));
  return request;
}

ua::HistoryReadRequest ToWireEventsRequest(
    const HistoryReadEventsDetails& details) {
  ua::ReadEventDetails events;
  events.num_values_per_node = 0;
  events.start_time = details.from;
  events.end_time = details.to;
  events.filter = ToUaEventFilter(DefaultEventFieldPaths(), details.filter);

  ua::HistoryReadRequest request;
  request.history_read_details = ua::ToExtensionObject(events);
  request.timestamps_to_return = ua::TimestampsToReturn::Both;
  ua::HistoryReadValueId node;
  node.node_id = details.node_id;
  request.nodes_to_read.push_back(std::move(node));
  return request;
}

StatusOr<HistoryReadRawResult> ToManagedRawResult(
    const ua::HistoryReadResponse& wire) {
  if (wire.results.empty()) {
    return Status{wire.response_header.service_result};
  }
  const auto& wire_result = wire.results.front();
  // A non-Good per-node status is the operation's failure; the wire carries no
  // data with it. OPC UA Part 4 §5.11.3 HistoryRead,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.11.3
  if (const Status status{wire_result.status_code}; !status) {
    return status;
  }
  HistoryReadRawResult result;
  result.continuation_point = wire_result.continuation_point;
  ua::HistoryData data;
  if (ua::FromExtensionObject(wire_result.history_data, data))
    result.values = std::move(data.data_values);
  return result;
}

HistoryReadEventsResult ToManagedEventsResult(
    const ua::HistoryReadResponse& wire) {
  HistoryReadEventsResult result;
  if (wire.results.empty()) {
    result.status = wire.response_header.service_result;
    return result;
  }
  const auto& wire_result = wire.results.front();
  result.status = wire_result.status_code;
  ua::HistoryEvent history_event;
  if (ua::FromExtensionObject(wire_result.history_data, history_event)) {
    const auto& field_paths = DefaultEventFieldPaths();
    result.events.reserve(history_event.events.size());
    for (const auto& list : history_event.events)
      result.events.push_back(
          ReconstructEventFromFields(field_paths, list.event_fields));
  }
  return result;
}

bool IsEventsRequest(const ua::HistoryReadRequest& wire) {
  ua::ReadEventDetails events;
  return ua::FromExtensionObject(wire.history_read_details, events);
}

bool IsEventsResponse(const ua::HistoryReadResponse& wire) {
  if (wire.results.empty())
    return false;
  ua::HistoryEvent history_event;
  return ua::FromExtensionObject(wire.results.front().history_data,
                                 history_event);
}

std::optional<HistoryUpdateDetails> ToManaged(
    const ua::HistoryUpdateRequest& wire) {
  // A single UpdateDataDetails or UpdateEventDetails element per request.
  if (wire.history_update_details.size() != 1)
    return std::nullopt;
  const auto& detail = wire.history_update_details.front();

  ua::UpdateDataDetails data;
  if (ua::FromExtensionObject(detail, data)) {
    UpdateDataDetails managed;
    managed.node_id = data.node_id;
    managed.perform_insert_replace =
        CastEnum<PerformUpdateType>(data.perform_insert_replace);
    managed.values = std::move(data.update_values);
    return HistoryUpdateDetails{std::move(managed)};
  }

  ua::UpdateEventDetails events;
  if (ua::FromExtensionObject(detail, events)) {
    UpdateEventDetails managed;
    managed.node_id = events.node_id;
    managed.perform_insert_replace =
        CastEnum<PerformUpdateType>(events.perform_insert_replace);
    const auto& field_paths = DefaultEventFieldPaths();
    managed.events.reserve(events.event_data.size());
    for (const auto& list : events.event_data)
      managed.events.push_back(
          ReconstructEventFromFields(field_paths, list.event_fields));
    return HistoryUpdateDetails{std::move(managed)};
  }

  return std::nullopt;
}

ua::HistoryUpdateResponse ToWire(const HistoryUpdateResult& result) {
  ua::HistoryUpdateResult wire_result;
  wire_result.status_code = result.status;
  wire_result.operation_results.reserve(result.operation_results.size());
  for (const auto& status : result.operation_results)
    wire_result.operation_results.push_back(Status{status});
  ua::HistoryUpdateResponse response;
  response.results.push_back(std::move(wire_result));
  return response;
}

ua::HistoryUpdateRequest ToWire(const HistoryUpdateDetails& details) {
  ua::HistoryUpdateRequest request;
  if (const auto* data = std::get_if<UpdateDataDetails>(&details)) {
    ua::UpdateDataDetails wire_data;
    wire_data.node_id = data->node_id;
    wire_data.perform_insert_replace =
        CastEnum<ua::PerformUpdateType>(data->perform_insert_replace);
    wire_data.update_values = data->values;
    request.history_update_details.push_back(ua::ToExtensionObject(wire_data));
  } else {
    const auto& events = std::get<UpdateEventDetails>(details);
    ua::UpdateEventDetails wire_events;
    wire_events.node_id = events.node_id;
    wire_events.perform_insert_replace =
        CastEnum<ua::PerformUpdateType>(events.perform_insert_replace);
    const auto& field_paths = DefaultEventFieldPaths();
    wire_events.event_data.reserve(events.events.size());
    for (const auto& event : events.events) {
      ua::HistoryEventFieldList list;
      list.event_fields = ProjectEventFields(field_paths, std::any{event});
      wire_events.event_data.push_back(std::move(list));
    }
    request.history_update_details.push_back(
        ua::ToExtensionObject(wire_events));
  }
  return request;
}

HistoryUpdateResult ToManaged(const ua::HistoryUpdateResponse& wire) {
  HistoryUpdateResult result;
  if (wire.results.empty()) {
    result.status = wire.response_header.service_result;
    return result;
  }
  const auto& wire_result = wire.results.front();
  result.status = wire_result.status_code;
  result.operation_results.reserve(wire_result.operation_results.size());
  for (const auto& status : wire_result.operation_results)
    result.operation_results.push_back(status.code());
  return result;
}

}  // namespace opcua::history_conversion
