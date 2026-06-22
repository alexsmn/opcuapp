#pragma once

#include "opcua/message.h"
#include "opcua/services/service_message.h"
#include "opcua/types/status_or.h"

#include <boost/json/value.hpp>

namespace opcua::ws {

boost::json::value EncodeJson(const ServiceRequest& request);
boost::json::value EncodeJson(const ServiceResponse& response);
boost::json::value EncodeJson(const RequestMessage& request);
boost::json::value EncodeJson(const ResponseMessage& response);

StatusOr<ServiceRequest> DecodeServiceRequest(const boost::json::value& json);
StatusOr<ServiceResponse> DecodeServiceResponse(const boost::json::value& json);
StatusOr<RequestMessage> DecodeRequestMessage(const boost::json::value& json);
StatusOr<ResponseMessage> DecodeResponseMessage(const boost::json::value& json);

}  // namespace opcua::ws
