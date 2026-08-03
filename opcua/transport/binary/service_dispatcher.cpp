#include "opcua/transport/binary/service_dispatcher.h"

#include "opcua/base/boost_log.h"
#include "opcua/transport/binary/service_codec.h"

namespace opcua::binary {
namespace {

BoostLogger logger_{LOG_NAME("OpcUaServiceDispatcher")};

std::string_view RequestName(const RequestBody& request) {
  return std::visit(
      [](const auto& typed_request) -> std::string_view {
        using Request = std::decay_t<decltype(typed_request)>;
        // A pure-ua request carries its operation name as a member; the domain
        // requests (discovery / session / subscription) keep an explicit label.
        if constexpr (requires { Request::kServiceName; }) {
          return Request::kServiceName;
        } else if constexpr (std::is_same_v<Request, FindServersRequest>) {
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
        } else if constexpr (std::is_same_v<Request, PublishRequest>) {
          return "Publish";
        } else if constexpr (std::is_same_v<Request, RepublishRequest>) {
          return "Republish";
        } else if constexpr (std::is_same_v<Request,
                                            CreateMonitoredItemsRequest>) {
          return "CreateMonitoredItems";
        } else if constexpr (std::is_same_v<Request,
                                            ModifyMonitoredItemsRequest>) {
          return "ModifyMonitoredItems";
        }
        return "";
      },
      request);
}

std::optional<std::vector<char>> EncodeResponse(UInt32 request_handle,
                                                const ResponseBody& response) {
  return EncodeServiceResponse(request_handle, response);
}

// Hex of the first bytes of an undecodable request payload — enough to
// identify the service TypeId and the header shape when diagnosing interop
// with third-party clients, without dumping whole messages into the log.
std::string HexPrefix(const std::vector<char>& payload) {
  constexpr std::size_t kMaxBytes = 96;
  const std::size_t count = std::min(payload.size(), kMaxBytes);
  std::string hex;
  hex.reserve(count * 2 + 1);
  constexpr char kDigits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count; ++i) {
    const auto byte = static_cast<std::uint8_t>(payload[i]);
    hex.push_back(kDigits[byte >> 4]);
    hex.push_back(kDigits[byte & 0x0f]);
  }
  if (payload.size() > kMaxBytes) {
    hex.push_back('+');
  }
  return hex;
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
                           << LOG_TAG("RequestHandle", *request_handle)
                           << LOG_TAG("PayloadPrefix", HexPrefix(payload))
                           << LOG_TAG("Peer", connection_.peer);
      co_return EncodeServiceResponse(
          *request_handle, ResponseBody{ServiceFault{
                               .status = StatusCode::Bad_ServiceUnsupported}});
    }
    LOG_WARNING(logger_) << "OPC UA binary request decode failed"
                         << LOG_TAG("Peer", connection_.peer);
    co_return std::nullopt;
  }

  auto response = co_await runtime_.HandleDecodedRequest(connection_, *request);
  if (!response.has_value()) {
    const auto request_name = RequestName(request->body);
    LOG_WARNING(logger_) << "OPC UA binary request handling failed: "
                         << request_name
                         << LOG_TAG("RequestHandle",
                                    request->header.request_handle)
                         << LOG_TAG("Peer", connection_.peer);
    co_return std::nullopt;
  }

  auto encoded = EncodeResponse(request->header.request_handle, *response);
  if (!encoded.has_value()) {
    const auto request_name = RequestName(request->body);
    LOG_WARNING(logger_) << "OPC UA binary response encode failed: "
                         << request_name
                         << LOG_TAG("RequestHandle",
                                    request->header.request_handle)
                         << LOG_TAG("Peer", connection_.peer);
  }
  co_return encoded;
}

}  // namespace opcua::binary
