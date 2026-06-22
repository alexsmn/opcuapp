#include "opcua/server/service_handler.h"

#include "opcua/base/boost_log.h"
#include "opcua/base/debug_util.h"
#include "opcua/base/time/time.h"

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

ServiceContext MakeServiceContext(const NodeId& user_id,
                                  ServiceContext base_context = {}) {
  return base_context.with_user_id(user_id);
}

DataValue NormalizeReadResult(DataValue result) {
  constexpr unsigned kBadNodeIdUnknownFullCode = 0x80340000u;
  if (result.status_code == StatusCode::Bad_WrongNodeId) {
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
        if constexpr (std::is_same_v<T, ReadRequest>) {
          co_return co_await HandleRead(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, WriteRequest>) {
          co_return co_await HandleWrite(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, BrowseRequest>) {
          co_return co_await HandleBrowse(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, BrowseNextRequest>) {
          co_return ServiceResponse{
              BrowseNextResponse{.status = StatusCode::Bad}};
        } else if constexpr (std::is_same_v<T, TranslateBrowsePathsRequest>) {
          co_return co_await HandleTranslateBrowsePaths(
              std::move(typed_request));
        } else if constexpr (std::is_same_v<T, CallRequest>) {
          co_return co_await HandleCall(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, HistoryReadRawRequest>) {
          co_return co_await HandleHistoryReadRaw(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, HistoryReadEventsRequest>) {
          co_return co_await HandleHistoryReadEvents(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, HistoryUpdateRequest>) {
          co_return co_await HandleHistoryUpdate(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, AddNodesRequest>) {
          co_return co_await HandleAddNodes(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, DeleteNodesRequest>) {
          co_return co_await HandleDeleteNodes(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, AddReferencesRequest>) {
          co_return co_await HandleAddReferences(std::move(typed_request));
        } else {
          co_return co_await HandleDeleteReferences(std::move(typed_request));
        }
      },
      std::move(request));
  co_return typed_response;
}

Awaitable<ServiceResponse> ServiceHandler::HandleRead(
    ReadRequest request) const {
  if (auto status = ValidateOperationCount(
          request.inputs.size(), operation_limits.max_nodes_per_read)) {
    co_return ServiceResponse{ReadResponse{.status = *status}};
  }
  if (request.timestamps_to_return > kTimestampsNeither) {
    co_return ServiceResponse{
        ReadResponse{.status = StatusCode::Bad_TimestampsToReturnInvalid}};
  }
  const auto input_count = request.inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result =
      co_await callbacks.read(MakeServiceContext(user_id),
                              std::make_shared<const std::vector<ReadValueId>>(
                                  std::move(request.inputs)));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  results = NormalizeReadResults(std::move(results));
  ApplyTimestampsToReturn(results, request.timestamps_to_return);
  const auto duration = base::TimeTicks::Now() - start_ticks;
  LOG_INFO(logger_) << "OPC UA Read completed"
                    << LOG_TAG("InputCount", input_count)
                    << LOG_TAG("ResultCount", results.size())
                    << LOG_TAG("DurationMs", duration.InMilliseconds())
                    << LOG_TAG("Status", ToString(status));
  co_return ServiceResponse{
      ReadResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleWrite(
    WriteRequest request) const {
  if (auto status = ValidateOperationCount(
          request.inputs.size(), operation_limits.max_nodes_per_write)) {
    co_return ServiceResponse{WriteResponse{.status = *status}};
  }
  auto result =
      co_await callbacks.write(MakeServiceContext(user_id),
                               std::make_shared<const std::vector<WriteValue>>(
                                   std::move(request.inputs)));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      WriteResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleBrowse(
    BrowseRequest request) const {
  if (auto status = ValidateOperationCount(
          request.inputs.size(), operation_limits.max_nodes_per_browse)) {
    co_return ServiceResponse{BrowseResponse{.status = *status}};
  }
  // The server exposes no Views, so any non-null view id is unknown. OPC UA
  // Part 4 §5.8.2 Browse,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.2
  if (!request.view_id.is_null()) {
    co_return ServiceResponse{
        BrowseResponse{.status = StatusCode::Bad_ViewIdUnknown}};
  }
  const auto input_count = request.inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await callbacks.browse(MakeServiceContext(user_id),
                                          std::move(request.inputs));
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
                    << LOG_TAG("Status", ToString(status));
  co_return ServiceResponse{
      BrowseResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleTranslateBrowsePaths(
    TranslateBrowsePathsRequest request) const {
  if (auto status = ValidateOperationCount(
          request.inputs.size(),
          operation_limits.max_nodes_per_translate_browse_paths_to_node_ids)) {
    co_return ServiceResponse{TranslateBrowsePathsResponse{.status = *status}};
  }
  auto result =
      co_await callbacks.translate_browse_paths(std::move(request.inputs));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      TranslateBrowsePathsResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleCall(
    CallRequest request) const {
  if (auto status = ValidateOperationCount(
          request.methods.size(), operation_limits.max_nodes_per_method_call)) {
    co_return ServiceResponse{CallResponse{.status = *status}};
  }
  CallResponse response;
  response.results.reserve(request.methods.size());
  for (auto& method : request.methods) {
    auto status = co_await callbacks.call(std::move(method.object_id),
                                          std::move(method.method_id),
                                          std::move(method.arguments), user_id);
    response.results.push_back(MethodCallResult{std::move(status)});
  }

  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryReadRaw(
    HistoryReadRawRequest request) const {
  // OPC UA Part 11 §6.4.3 ReadRawModifiedDetails: a raw read must bound the
  // data by a time range or continue an existing read; with neither a start nor
  // end time and no continuation point the details are invalid.
  // https://reference.opcfoundation.org/Core/Part11/v105/docs/6.4.3
  if (request.details.from.is_null() && request.details.to.is_null() &&
      request.details.continuation_point.empty() &&
      !request.details.release_continuation_point) {
    co_return ServiceResponse{HistoryReadRawResponse{HistoryReadRawResult{
        .status = StatusCode::Bad_HistoryOperationInvalid}}};
  }
  auto result = co_await callbacks.history_read_raw(std::move(request.details));
  co_return ServiceResponse{HistoryReadRawResponse{std::move(result)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryReadEvents(
    HistoryReadEventsRequest request) const {
  auto result = co_await callbacks.history_read_events(
      std::move(request.details.node_id), request.details.from,
      request.details.to, std::move(request.details.filter));
  co_return ServiceResponse{HistoryReadEventsResponse{std::move(result)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryUpdate(
    HistoryUpdateRequest request) const {
  auto result = co_await callbacks.history_update(std::move(request.details));
  co_return ServiceResponse{HistoryUpdateResponse{std::move(result)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleAddNodes(
    AddNodesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.items.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{AddNodesResponse{.status = *status}};
  }
  auto result = co_await callbacks.add_nodes(std::move(request.items));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      AddNodesResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleDeleteNodes(
    DeleteNodesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.items.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{DeleteNodesResponse{.status = *status}};
  }
  auto result = co_await callbacks.delete_nodes(std::move(request.items));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      DeleteNodesResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleAddReferences(
    AddReferencesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.items.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{AddReferencesResponse{.status = *status}};
  }
  auto result = co_await callbacks.add_references(std::move(request.items));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      AddReferencesResponse{std::move(status), std::move(results)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleDeleteReferences(
    DeleteReferencesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.items.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{DeleteReferencesResponse{.status = *status}};
  }
  auto result = co_await callbacks.delete_references(std::move(request.items));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      DeleteReferencesResponse{std::move(status), std::move(results)}};
}

}  // namespace opcua
