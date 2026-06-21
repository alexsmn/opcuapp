#include "opcua/binary/service_dispatcher.h"

#include "opcua/base/boost_log.h"
#include "opcua/binary/service_codec.h"

namespace opcua::binary {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaServiceDispatcher")};

const char* RequestName(const RequestBody& request) {
  return std::visit(
      [](const auto& typed_request) -> const char* {
        using Request = std::decay_t<decltype(typed_request)>;
        if constexpr (std::is_same_v<Request, FindServersRequest>) {
          return "FindServers";
        } else if constexpr (std::is_same_v<Request, GetEndpointsRequest>) {
          return "GetEndpoints";
        } else if constexpr (std::is_same_v<Request, CreateSessionRequest>) {
          return "CreateSession";
        } else if constexpr (std::is_same_v<Request, ActivateSessionRequest>) {
          return "ActivateSession";
        } else if constexpr (std::is_same_v<Request, CloseSessionRequest>) {
          return "CloseSession";
        } else if constexpr (std::is_same_v<Request,
                                            CreateSubscriptionRequest>) {
          return "CreateSubscription";
        } else if constexpr (std::is_same_v<Request,
                                            ModifySubscriptionRequest>) {
          return "ModifySubscription";
        } else if constexpr (std::is_same_v<Request,
                                            SetPublishingModeRequest>) {
          return "SetPublishingMode";
        } else if constexpr (std::is_same_v<Request,
                                            DeleteSubscriptionsRequest>) {
          return "DeleteSubscriptions";
        } else if constexpr (std::is_same_v<Request, PublishRequest>) {
          return "Publish";
        } else if constexpr (std::is_same_v<Request, RepublishRequest>) {
          return "Republish";
        } else if constexpr (std::is_same_v<Request,
                                            TransferSubscriptionsRequest>) {
          return "TransferSubscriptions";
        } else if constexpr (std::is_same_v<Request,
                                            CreateMonitoredItemsRequest>) {
          return "CreateMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            ModifyMonitoredItemsRequest>) {
          return "ModifyMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            DeleteMonitoredItemsRequest>) {
          return "DeleteMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            SetMonitoringModeRequest>) {
          return "SetMonitoringMode";
        } else if constexpr (std::is_same_v<Request, ReadRequest>) {
          return "Read";
        } else if constexpr (std::is_same_v<Request, WriteRequest>) {
          return "Write";
        } else if constexpr (std::is_same_v<Request, BrowseRequest>) {
          return "Browse";
        } else if constexpr (std::is_same_v<Request, BrowseNextRequest>) {
          return "BrowseNext";
        } else if constexpr (std::is_same_v<Request,
                                            TranslateBrowsePathsRequest>) {
          return "TranslateBrowsePaths";
        } else if constexpr (std::is_same_v<Request, CallRequest>) {
          return "Call";
        } else if constexpr (std::is_same_v<Request, HistoryReadRawRequest>) {
          return "HistoryReadRaw";
        } else if constexpr (std::is_same_v<Request,
                                            HistoryReadEventsRequest>) {
          return "HistoryReadEvents";
        } else if constexpr (std::is_same_v<Request, AddNodesRequest>) {
          return "AddNodes";
        } else if constexpr (std::is_same_v<Request, DeleteNodesRequest>) {
          return "DeleteNodes";
        } else if constexpr (std::is_same_v<Request, AddReferencesRequest>) {
          return "AddReferences";
        } else if constexpr (std::is_same_v<Request, DeleteReferencesRequest>) {
          return "DeleteReferences";
        }
      },
      request);
}

std::optional<std::vector<char>> EncodeResponse(UInt32 request_handle,
                                                const ResponseBody& response) {
  return EncodeServiceResponse(request_handle, response);
}

std::optional<std::vector<char>> EncodeResponse(
    UInt32 request_handle,
    const std::vector<std::vector<std::string>>& history_event_field_paths,
    const ResponseBody& response) {
  return std::visit(
      [request_handle, &history_event_field_paths](
          const auto& typed_response) -> std::optional<std::vector<char>> {
        using T = std::decay_t<decltype(typed_response)>;
        if constexpr (std::is_same_v<T, HistoryReadEventsResponse>) {
          return EncodeHistoryReadEventsResponse(request_handle, typed_response,
                                                 history_event_field_paths);
        } else {
          return EncodeResponse(request_handle, ResponseBody{typed_response});
        }
      },
      response);
}
}  // namespace

ServiceDispatcher::ServiceDispatcher(Context context)
    : runtime_{context.runtime}, connection_{context.connection} {}

Awaitable<std::optional<std::vector<char>>> ServiceDispatcher::HandlePayload(
    std::vector<char> payload) {
  const auto request = DecodeServiceRequest(payload);
  if (!request.has_value()) {
    // Unknown/unsupported (or undecodable) service: answer with a ServiceFault
    // carrying the request handle so the client can correlate, instead of
    // silently dropping the channel. OPC UA Part 4 §7.34 ServiceFault.
    if (const auto request_handle = DecodeRequestHandle(payload)) {
      LOG_WARNING(logger_) << "OPC UA binary unsupported service request"
                           << LOG_TAG("RequestHandle", *request_handle);
      co_return EncodeServiceResponse(
          *request_handle,
          ResponseBody{ServiceFault{
              .status = StatusCode::Bad_ServiceUnsupported}});
    }
    LOG_WARNING(logger_) << "OPC UA binary request decode failed";
    co_return std::nullopt;
  }

  auto response = co_await runtime_.HandleDecodedRequest(connection_, *request);
  if (!response.has_value()) {
    const auto request_name = RequestName(request->body);
    LOG_WARNING(logger_) << "OPC UA binary request handling failed: "
                         << request_name
                         << LOG_TAG("RequestHandle",
                                    request->header.request_handle);
    co_return std::nullopt;
  }

  auto encoded = EncodeResponse(request->header.request_handle,
                                request->history_event_field_paths, *response);
  if (!encoded.has_value()) {
    const auto request_name = RequestName(request->body);
    LOG_WARNING(logger_) << "OPC UA binary response encode failed: "
                         << request_name
                         << LOG_TAG("RequestHandle",
                                    request->header.request_handle);
  }
  co_return encoded;
}

}  // namespace opcua::binary
