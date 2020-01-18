#pragma once

#include <opcua.h>
#include <opcua_messagecontext.h>
#include <opcuapp/helpers.h>

namespace opcua {

// |target| must be created.
void CopyEncodeable(const OpcUa_EncodeableType& type,
                    const OpcUa_Void* source,
                    OpcUa_Void* target);

OPCUA_DEFINE_ENCODEABLE(AggregateFilter);
OPCUA_DEFINE_ENCODEABLE(ApplicationDescription);
OPCUA_DEFINE_ENCODEABLE(BrowseDescription);
OPCUA_DEFINE_ENCODEABLE(BrowseResult);
OPCUA_DEFINE_ENCODEABLE(BrowsePath);
OPCUA_DEFINE_ENCODEABLE(RelativePathElement);
OPCUA_DEFINE_ENCODEABLE(BrowsePathResult);
OPCUA_DEFINE_ENCODEABLE(BrowsePathTarget);
OPCUA_DEFINE_ENCODEABLE(EndpointDescription);
OPCUA_DEFINE_ENCODEABLE(MonitoredItemCreateRequest);
OPCUA_DEFINE_ENCODEABLE(MonitoredItemCreateResult);
OPCUA_DEFINE_ENCODEABLE(MonitoredItemNotification);
OPCUA_DEFINE_ENCODEABLE(MonitoringParameters);
OPCUA_DEFINE_ENCODEABLE(NotificationMessage);
OPCUA_DEFINE_ENCODEABLE(ReadValueId);
OPCUA_DEFINE_ENCODEABLE(ReferenceDescription);
OPCUA_DEFINE_ENCODEABLE(RequestHeader);
OPCUA_DEFINE_ENCODEABLE(ResponseHeader);
OPCUA_DEFINE_ENCODEABLE(UserTokenPolicy);
OPCUA_DEFINE_ENCODEABLE(DataChangeFilter);
OPCUA_DEFINE_ENCODEABLE(DataChangeNotification);
OPCUA_DEFINE_ENCODEABLE(EventFieldList);
OPCUA_DEFINE_ENCODEABLE(EventNotificationList);
OPCUA_DEFINE_ENCODEABLE(EventFilter);
OPCUA_DEFINE_ENCODEABLE(ServerStatusDataType);

}  // namespace opcua

#include <opcuapp/encodable_object.h>
