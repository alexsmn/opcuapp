#include "opcua/service_handler.h"

#include "opcua/base/boost_log.h"
#include "opcua/base/time/time.h"
#include "opcua/endpoint_core.h"

#include "opcua/scada/coroutine_services.h"

#include "opcua/base/debug_util.h"

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
std::optional<scada::Status> ValidateOperationCount(std::size_t count,
                                                    std::uint32_t limit) {
  if (count == 0) {
    return scada::Status{scada::StatusCode::Bad_NothingToDo};
  }
  if (count > limit) {
    return scada::Status{scada::StatusCode::Bad_TooManyOperations};
  }
  return std::nullopt;
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
              BrowseNextResponse{.status = scada::StatusCode::Bad}};
        } else if constexpr (std::is_same_v<T, TranslateBrowsePathsRequest>) {
          co_return co_await HandleTranslateBrowsePaths(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, CallRequest>) {
          co_return co_await HandleCall(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, HistoryReadRawRequest>) {
          co_return co_await HandleHistoryReadRaw(std::move(typed_request));
        } else if constexpr (std::is_same_v<T, HistoryReadEventsRequest>) {
          co_return co_await HandleHistoryReadEvents(std::move(typed_request));
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
  const auto input_count = request.inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await attribute_service.Read(
      MakeServiceContext(user_id),
      std::make_shared<const std::vector<scada::ReadValueId>>(
          std::move(request.inputs)));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  results = NormalizeReadResults(std::move(results));
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
  auto result = co_await attribute_service.Write(
      MakeServiceContext(user_id),
      std::make_shared<const std::vector<scada::WriteValue>>(
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
  const auto input_count = request.inputs.size();
  const auto start_ticks = base::TimeTicks::Now();
  auto result = co_await view_service.Browse(
      MakeServiceContext(user_id), std::move(request.inputs));
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

Awaitable<ServiceResponse>
ServiceHandler::HandleTranslateBrowsePaths(
    TranslateBrowsePathsRequest request) const {
  if (auto status = ValidateOperationCount(
          request.inputs.size(),
          operation_limits.max_nodes_per_translate_browse_paths_to_node_ids)) {
    co_return ServiceResponse{TranslateBrowsePathsResponse{.status = *status}};
  }
  auto result = co_await view_service.TranslateBrowsePaths(
      std::move(request.inputs));
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
    auto status = co_await method_service.Call(
        std::move(method.object_id), std::move(method.method_id),
        std::move(method.arguments), user_id);
    response.results.push_back(MethodCallResult{std::move(status)});
  }

  co_return ServiceResponse{std::move(response)};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryReadRaw(
    HistoryReadRawRequest request) const {
  auto result = co_await history_service.HistoryReadRaw(
      std::move(request.details));
  co_return ServiceResponse{HistoryReadRawResponse{std::move(result)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleHistoryReadEvents(
    HistoryReadEventsRequest request) const {
  auto result = co_await history_service.HistoryReadEvents(
      std::move(request.details.node_id), request.details.from,
      request.details.to, std::move(request.details.filter));
  co_return ServiceResponse{
      HistoryReadEventsResponse{std::move(result)}};
}

Awaitable<ServiceResponse> ServiceHandler::HandleAddNodes(
    AddNodesRequest request) const {
  if (auto status = ValidateOperationCount(
          request.items.size(),
          operation_limits.max_nodes_per_node_management)) {
    co_return ServiceResponse{AddNodesResponse{.status = *status}};
  }
  auto result = co_await node_management_service.AddNodes(
      std::move(request.items));
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
  auto result =
      co_await node_management_service.DeleteNodes(std::move(request.items));
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
  auto result =
      co_await node_management_service.AddReferences(std::move(request.items));
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
  auto result = co_await node_management_service.DeleteReferences(
      std::move(request.items));
  auto status = result.status();
  auto results = std::move(result).value_or({});
  co_return ServiceResponse{
      DeleteReferencesResponse{std::move(status), std::move(results)}};
}

}  // namespace opcua
