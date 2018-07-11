#pragma once

#include <opcuapp/structs.h>

namespace opcua {

OPCUA_DEFINE_ENCODEABLE(ActivateSessionRequest);
OPCUA_DEFINE_ENCODEABLE(ActivateSessionResponse);
OPCUA_DEFINE_ENCODEABLE(BrowseRequest);
OPCUA_DEFINE_ENCODEABLE(BrowseResponse);
OPCUA_DEFINE_ENCODEABLE(CloseSessionRequest);
OPCUA_DEFINE_ENCODEABLE(CloseSessionResponse);
OPCUA_DEFINE_ENCODEABLE(CreateSessionRequest);
OPCUA_DEFINE_ENCODEABLE(CreateSessionResponse);
OPCUA_DEFINE_ENCODEABLE(CreateSubscriptionRequest);
OPCUA_DEFINE_ENCODEABLE(CreateSubscriptionResponse);
OPCUA_DEFINE_ENCODEABLE(CreateMonitoredItemsRequest);
OPCUA_DEFINE_ENCODEABLE(CreateMonitoredItemsResponse);
OPCUA_DEFINE_ENCODEABLE(DeleteMonitoredItemsRequest);
OPCUA_DEFINE_ENCODEABLE(DeleteMonitoredItemsResponse);
OPCUA_DEFINE_ENCODEABLE(GetEndpointsRequest);
OPCUA_DEFINE_ENCODEABLE(GetEndpointsResponse);
OPCUA_DEFINE_ENCODEABLE(FindServersRequest);
OPCUA_DEFINE_ENCODEABLE(FindServersResponse);
OPCUA_DEFINE_ENCODEABLE(PublishRequest);
OPCUA_DEFINE_ENCODEABLE(PublishResponse);
OPCUA_DEFINE_ENCODEABLE(ReadRequest);
OPCUA_DEFINE_ENCODEABLE(ReadResponse);

}  // namespace opcua
