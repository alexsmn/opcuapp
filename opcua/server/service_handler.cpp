#include "opcua/server/service_handler.h"

#include "opcua/base/boost_log.h"
#include "opcua/base/debug_util.h"
#include "opcua/base/time_ticks.h"
#include "opcua/services/browse_conversion.h"
#include "opcua/services/history_conversion.h"
#include "opcua/services/node_attributes_conversion.h"
#include "opcua/types/date_time.h"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace opcua {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaServiceHandler")};

// Service-level validation of an operation array's size (OPC UA Part 4 §5.10):
// an empty array is Bad_NothingToDo, an array larger than the advertised
// OperationLimit is Bad_TooManyOperations. Returns nullopt when the size is
// acceptable and the request should proceed to per-operation processing.
// TimestampsToReturn raw enumeration values (OPC UA Part 4 §7.40,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40).
constexpr std::uint32_t kTimestampsSource = 0;
constexpr std::uint32_t kTimestampsServer = 1;
constexpr std::uint32_t kTimestampsNeither = 3;

// Strips the source and/or server timestamp from each read result so the
// response carries only the timestamps the client requested via
// TimestampsToReturn. The DataValue encoder omits null timestamps.
void ApplyTimestampsToReturn(std::vector<DataValue>& results,
                             std::uint32_t timestamps_to_return) {
  for (auto& value : results) {
    if (timestamps_to_return == kTimestampsServer ||
        timestamps_to_return == kTimestampsNeither) {
      value.source_timestamp = DateTime{};
    }
    if (timestamps_to_return == kTimestampsSource ||
        timestamps_to_return == kTimestampsNeither) {
      value.server_timestamp = DateTime{};
    }
  }
}

// Renders the caller's user id for the request logs' UserId tag. Empty for an
// anonymous session so sinks drop the attribute instead of printing the null
// id ("i=0").
std::string UserIdTag(const ServiceContext& context) {
  return context.user_id().is_null() ? std::string{}
                                     : context.user_id().ToString();
}

std::optional<Status> ValidateOperationCount(std::size_t count,
                                             std::uint32_t limit) {
  if (count == 0) {
    return Status{StatusCode::Bad_NothingToDo};
  }
  if (count > limit) {
    return Status{StatusCode::Bad_TooManyOperations};
  }
  return std::nullopt;
}

DataValue NormalizeReadResult(DataValue result) {
  constexpr unsigned kBadNodeIdUnknownFullCode = 0x80340000u;
  if (result.status_code == StatusCode::Bad_NodeIdUnknown) {
    result.status_code = Status::FromFullCode(kBadNodeIdUnknownFullCode).code();
  }
  return result;
}

std::vector<DataValue> NormalizeReadResults(std::vector<DataValue> results) {
  for (auto& result : results) {
    result = NormalizeReadResult(std::move(result));
  }
  return results;
}

}  // namespace

ServiceHandler::ServiceHandler(ServiceHandlerContext&& context)
    : ServiceHandlerContext{std::move(context)} {}

Awaitable<ServiceResponse> ServiceHandler::Handle(
    ServiceRequest request) const {
  auto typed_response = co_await std::visit(
      [this](auto&& typed_request) -> Awaitable<ServiceResponse> {
        using T = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<T, ua::ReadRequest>) {
          co_return co_await HandleRead(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::WriteRequest>) {
          co_return co_await HandleWrite(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::BrowseRequest>) {
          co_return co_await HandleBrowse(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::BrowseNextRequest>) {
          co_return ServiceResponse{ua::BrowseNextResponse{
              .response_header = {.service_result = StatusCode::Bad}}};
        } else if constexpr (std::is_same_v<
                                 T, ua::TranslateBrowsePathsToNodeIdsRequest>) {
          co_return co_await HandleTranslateBrowsePaths(
              std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::CallRequest>) {
          co_return co_await HandleCall(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::HistoryReadRequest>) {
          co_return co_await HandleHistoryRead(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::HistoryUpdateRequest>) {
          co_return co_await HandleHistoryUpdate(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::AddNodesRequest>) {
          co_return co_await HandleAddNodes(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::DeleteNodesRequest>) {
          co_return co_await HandleDeleteNodes(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, ua::AddReferencesRequest>) {
          co_return co_await HandleAddReferences(std::move(typed_request));
        } else {
          co_return co_await HandleDeleteReferences(std::move(typed_request));
        }
      },
      std::move(request));
  co_return typed_response;
}

Awaitable<ServiceResponse> ServiceHandler::HandleRead(
    ua::ReadRequest request) const {
  if (auto status = ValidateOperationCount(
          request.nodes_to_read.size(), operation_limits.max_nodes_per_read)) {
    co_return ServiceResponse{
        ua::ReadResponse{.response_header = {.service_result = *status}}};
  }
  // MaxAge and TimestampsToReturn are validated here (the codec no longer
  // rejects them): a negative MaxAge or an out-of-range TimestampsToReturn is a
  // service-level fault (OPC UA Part 4 §7.40, §5.10.2).
  const std::uint32_t timestamps_to_return =
      static_cast<std::uint32_t>(request.timestamps_to_return);
  if (request.max_age < 0 || timestamps_to_return > kTimestampsNeither) {
    co_return ServiceResponse{ua::ReadResponse{
        .response_header = {.service_result =
                                StatusCode::Bad_TimestampsToReturnInvalid}}};
  }
  // The read callback keeps the hand-written ReadValueId (client/bridge
  // vocabulary); IndexRange and DataEncoding are not modelled and are dropped.
  auto inputs = std::make_shared<std::vector<ReadValueId>>();
  inputs->reserve(request.nodes_to_read.size());
  for (const auto& value : request.nodes_to_read) {
    inputs->push_back(
        {.node_id = value.node_id,
         .attribute_id = static_cast<AttributeId>(value.attribute_id)});
  }
  const auto input_count = inputs->size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await callbacks.read(
      service_context,
      std::shared_ptr<const std::vector<ReadValueId>>(std::move(inputs)));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  results = NormalizeReadResults(std::move(results));
  ApplyTimestampsToReturn(results, timestamps_to_return);
  const auto duration = base::TimeTicks::Now() - start_ticks;
  // The trace tag ties this record to the caller's distributed trace in
  // structured log sinks (the context carries the request-header traceparent;
  // see ServerRuntime::HandleServiceRequest).
  LOG_INFO(logger_) << "OPC UA Read completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", results.size())
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(status))
                    << LOG_TAG("UserId", UserIdTag(service_context))
                    << LOG_TAG("Peer", service_context.peer())
                    << LOG_TAG(kTraceParentLogAttribute,
                               service_context.trace_id());
  ua::ReadResponse response;
  response.response_header.service_result = status;
  response.results = std::move(results);
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleWrite(
    ua::WriteRequest request) const {
  if (auto status =
          ValidateOperationCount(request.nodes_to_write.size(),
                                 operation_limits.max_nodes_per_write)) {
    co_return ServiceResponse{
        ua::WriteResponse{.response_header = {.service_result = *status}}};
  }
  // The write callback keeps the hand-written WriteValue (client/bridge
  // vocabulary): {node_id, attribute_id, value:Variant, flags}. Extract the
  // Variant from the generated WriteValue's DataValue; the wire never carried
  // flags (an opcuapp-internal detail) or index_range for this path, so both
  // are dropped without behavior change.
  auto inputs = std::make_shared<std::vector<WriteValue>>();
  inputs->reserve(request.nodes_to_write.size());
  for (auto& value : request.nodes_to_write) {
    inputs->push_back(
        {.node_id = std::move(value.node_id),
         .attribute_id = static_cast<AttributeId>(value.attribute_id),
         .value = std::move(value.value.value)});
  }
  const auto input_count = inputs->size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await callbacks.write(
      service_context,
      std::shared_ptr<const std::vector<WriteValue>>(std::move(inputs)));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  const auto duration = base::TimeTicks::Now() - start_ticks;
  LOG_INFO(logger_) << "OPC UA Write completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", results.size())
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(status))
                    << LOG_TAG("UserId", UserIdTag(service_context))
                    << LOG_TAG("Peer", service_context.peer())
                    << LOG_TAG(kTraceParentLogAttribute,
                               service_context.trace_id());
  ua::WriteResponse response;
  response.response_header.service_result = status;
  for (const auto status_code : results)
    response.results.push_back(Status{status_code});
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleBrowse(
    ua::BrowseRequest request) const {
  if (auto status =
          ValidateOperationCount(request.nodes_to_browse.size(),
                                 operation_limits.max_nodes_per_browse)) {
    co_return ServiceResponse{
        ua::BrowseResponse{.response_header = {.service_result = *status}}};
  }
  // The server exposes no Views, so any non-null view id is unknown. OPC UA
  // Part 4 §5.8.2 Browse,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.2
  if (!request.view.view_id.is_null()) {
    co_return ServiceResponse{ua::BrowseResponse{
        .response_header = {.service_result = StatusCode::Bad_ViewIdUnknown}}};
  }
  // The browse callback keeps the hand-written BrowseDescription/BrowseResult
  // (client/bridge vocabulary); convert to and from the generated types.
  std::vector<BrowseDescription> inputs;
  inputs.reserve(request.nodes_to_browse.size());
  for (const auto& description : request.nodes_to_browse)
    inputs.push_back(ToHandWritten(description));
  const auto input_count = inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await callbacks.browse(service_context, std::move(inputs));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  std::size_t reference_count = 0;
  for (const auto& browse_result : results) {
    reference_count += browse_result.references.size();
  }
  const auto duration = base::TimeTicks::Now() - start_ticks;
  LOG_INFO(logger_) << "OPC UA Browse completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", results.size())
                    << LOG_TAG("ReferenceCount", reference_count)
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(status))
                    << LOG_TAG("UserId", UserIdTag(service_context))
                    << LOG_TAG("Peer", service_context.peer())
                    << LOG_TAG(kTraceParentLogAttribute,
                               service_context.trace_id());
  ua::BrowseResponse response;
  response.response_header.service_result = status;
  response.results.reserve(results.size());
  for (const auto& browse_result : results)
    response.results.push_back(ToGenerated(browse_result));
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleTranslateBrowsePaths(
    ua::TranslateBrowsePathsToNodeIdsRequest request) const {
  if (auto status = ValidateOperationCount(
          request.browse_paths.size(),
          operation_limits.max_nodes_per_translate_browse_paths_to_node_ids)) {
    co_return ServiceResponse{ua::TranslateBrowsePathsToNodeIdsResponse{
        .response_header = {.service_result = *status}}};
  }
  // The callback keeps the hand-written BrowsePath/BrowsePathResult
  // (client/bridge vocabulary); convert to and from the generated types.
  std::vector<BrowsePath> inputs;
  inputs.reserve(request.browse_paths.size());
  for (const auto& path : request.browse_paths)
    inputs.push_back(ToHandWritten(path));
  auto result = co_await callbacks.translate_browse_paths(std::move(inputs));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  ua::TranslateBrowsePathsToNodeIdsResponse response;
  response.response_header.service_result = status;
  response.results.reserve(results.size());
  for (const auto& path_result : results)
    response.results.push_back(ToGenerated(path_result));
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleCall(
    ua::CallRequest request) const {
  if (auto status =
          ValidateOperationCount(request.methods_to_call.size(),
                                 operation_limits.max_nodes_per_method_call)) {
    co_return ServiceResponse{
        ua::CallResponse{.response_header = {.service_result = *status}}};
  }
  const auto input_count = request.methods_to_call.size();
  const auto start_ticks = base::TimeTicks::Now();
  std::size_t good_count = 0;
  ua::CallResponse response;
  response.results.reserve(request.methods_to_call.size());
  for (auto& method : request.methods_to_call) {
    auto status = co_await callbacks.call(
        std::move(method.object_id), std::move(method.method_id),
        std::move(method.input_arguments), service_context);
    if (status) {
      ++good_count;
    }
    response.results.push_back(ua::CallMethodResult{.status_code = status});
  }
  const auto duration = base::TimeTicks::Now() - start_ticks;
  // Call has no service-level status; GoodCount summarizes the per-method
  // results instead.
  LOG_INFO(logger_) << "OPC UA Call completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", response.results.size())
                    << LOG_TAG("GoodCount", good_count)
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("UserId", UserIdTag(service_context))
                    << LOG_TAG("Peer", service_context.peer())
                    << LOG_TAG(kTraceParentLogAttribute,
                               service_context.trace_id());

  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryRead(
    ua::HistoryReadRequest request) const {
  // Decode the details ExtensionObject into the managed raw/events read (the
  // callbacks keep the hand-written history vocabulary). An unsupported request
  // (multi-node, bad details, ...) yields a service-level fault.
  auto decoded = history_conversion::ToManaged(request);
  if (!decoded) {
    co_return ServiceResponse{ua::HistoryReadResponse{
        .response_header = {.service_result =
                                StatusCode::Bad_HistoryOperationInvalid}}};
  }
  if (auto* raw = std::get_if<HistoryReadRawDetails>(&decoded->details)) {
    // OPC UA Part 11 §6.4.3 ReadRawModifiedDetails: a raw read must bound the
    // data by a time range or continue an existing read; with neither a start
    // nor end time and no continuation point the details are invalid.
    // https://reference.opcfoundation.org/Core/Part11/v105/docs/6.4.3
    if (raw->from.is_null() && raw->to.is_null() &&
        raw->continuation_point.empty() && !raw->release_continuation_point) {
      co_return ServiceResponse{history_conversion::ToWireRawResponse(
          Status{StatusCode::Bad_HistoryOperationInvalid})};
    }
    auto result = co_await callbacks.history_read_raw(std::move(*raw));
    co_return ServiceResponse{
        history_conversion::ToWireRawResponse(std::move(result))};
  }
  auto& events = std::get<HistoryReadEventsDetails>(decoded->details);
  auto result = co_await callbacks.history_read_events(
      std::move(events.node_id), events.from, events.to,
      std::move(events.filter));
  co_return ServiceResponse{history_conversion::ToWireEventsResponse(
      std::move(result), decoded->event_field_paths)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryUpdate(
    ua::HistoryUpdateRequest request) const {
  auto detail = history_conversion::ToManaged(request);
  if (!detail) {
    co_return ServiceResponse{ua::HistoryUpdateResponse{
        .response_header = {.service_result =
                                StatusCode::Bad_HistoryOperationInvalid}}};
  }
  // The wire detail is data (UpdateDataDetails) or event (UpdateEventDetails);
  // route each to its callback.
  StatusOr<std::vector<StatusCode>> result{StatusCode::Bad};
  if (auto* data = std::get_if<UpdateDataDetails>(&*detail)) {
    result =
        co_await callbacks.history_update(service_context, std::move(*data));
  } else {
    result = co_await callbacks.history_update_event(
        service_context, std::move(std::get<UpdateEventDetails>(*detail)));
  }
  co_return ServiceResponse{history_conversion::ToWire(std::move(result))};
}

Awaitable<ServiceResponse> ServiceHandler::HandleAddNodes(
    ua::AddNodesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.nodes_to_add.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{
        ua::AddNodesResponse{.response_header = {.service_result = *status}}};
  }
  // The add_nodes callback keeps the hand-written AddNodesItem (client/bridge
  // vocabulary): the ExpandedNodeId fields collapse to NodeId, BrowseName is
  // folded into the flat NodeAttributes, and the generated ExtensionObject body
  // is decoded via NodeAttributesToExtensionObject's inverse. The generated
  // reference_type_id is dropped (the hand-written item never modelled it).
  std::vector<AddNodesItem> items;
  items.reserve(request.nodes_to_add.size());
  for (auto& item : request.nodes_to_add) {
    const NodeClass node_class = static_cast<NodeClass>(item.node_class);
    NodeAttributes attributes =
        ExtensionObjectToNodeAttributes(node_class, item.node_attributes);
    attributes.set_browse_name(std::move(item.browse_name));
    items.push_back({.requested_id = item.requested_new_node_id.node_id(),
                     .parent_id = item.parent_node_id.node_id(),
                     .node_class = node_class,
                     .type_definition_id = item.type_definition.node_id(),
                     .attributes = std::move(attributes)});
  }
  auto result = co_await callbacks.add_nodes(service_context, std::move(items));
  ua::AddNodesResponse response;
  response.response_header.service_result = result.status();
  for (const auto& add_result : std::move(result).value_or({})) {
    response.results.push_back(
        ua::AddNodesResult{.status_code = Status{add_result.status_code},
                           .added_node_id = add_result.added_node_id});
  }
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleDeleteNodes(
    ua::DeleteNodesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.nodes_to_delete.size(),
          operation_limits.max_nodes_per_node_management)) {
    ua::DeleteNodesResponse response;
    response.response_header.service_result = Status{*status};
    co_return ServiceResponse{std::move(response)};
  }
  // The delete_nodes callback speaks the hand-written DeleteNodesItem (the
  // bridge vocabulary); convert from the generated request's items, which have
  // identical fields.
  std::vector<DeleteNodesItem> items;
  items.reserve(request.nodes_to_delete.size());
  for (const auto& item : request.nodes_to_delete) {
    items.push_back(
        {.node_id = item.node_id,
         .delete_target_references = item.delete_target_references});
  }
  auto result =
      co_await callbacks.delete_nodes(service_context, std::move(items));
  ua::DeleteNodesResponse response;
  response.response_header.service_result = result.status();
  for (const auto status_code : std::move(result).value_or({}))
    response.results.push_back(Status{status_code});
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleAddReferences(
    ua::AddReferencesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.references_to_add.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{ua::AddReferencesResponse{
        .response_header = {.service_result = *status}}};
  }
  // The add_references callback keeps the hand-written AddReferencesItem
  // (client/bridge vocabulary); convert from the generated request's items,
  // which have identical fields.
  std::vector<AddReferencesItem> items;
  items.reserve(request.references_to_add.size());
  for (const auto& item : request.references_to_add) {
    items.push_back(
        {.source_node_id = item.source_node_id,
         .reference_type_id = item.reference_type_id,
         .forward = item.is_forward,
         .target_server_uri = item.target_server_uri,
         .target_node_id = item.target_node_id,
         // The generated NodeClass (opcua::ua::NodeClass) and the hand-written
         // opcua::NodeClass share the standard integer values.
         .target_node_class = static_cast<NodeClass>(item.target_node_class)});
  }
  auto result =
      co_await callbacks.add_references(service_context, std::move(items));
  ua::AddReferencesResponse response;
  response.response_header.service_result = result.status();
  for (const auto status_code : std::move(result).value_or({}))
    response.results.push_back(Status{status_code});
  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleDeleteReferences(
    ua::DeleteReferencesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.references_to_delete.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{ua::DeleteReferencesResponse{
        .response_header = {.service_result = *status}}};
  }
  // The delete_references callback keeps the hand-written DeleteReferencesItem
  // (client/bridge vocabulary); convert from the generated request's items,
  // which have identical fields.
  std::vector<DeleteReferencesItem> items;
  items.reserve(request.references_to_delete.size());
  for (const auto& item : request.references_to_delete) {
    items.push_back({.source_node_id = item.source_node_id,
                     .reference_type_id = item.reference_type_id,
                     .forward = item.is_forward,
                     .target_node_id = item.target_node_id,
                     .delete_bidirectional = item.delete_bidirectional});
  }
  auto result =
      co_await callbacks.delete_references(service_context, std::move(items));
  ua::DeleteReferencesResponse response;
  response.response_header.service_result = result.status();
  for (const auto status_code : std::move(result).value_or({}))
    response.results.push_back(Status{status_code});
  co_return ServiceResponse{std::move(response)};
}

}  // namespace opcua
