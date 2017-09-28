#pragma once

#include "structs.h"

namespace opcua {

#define OPCUA_DEFINE_MESSAGE(Name) \
  struct Name : OpcUa_##Name { \
    OPCUA_DEFINE_MEMBERS(Name) \
    \
    static const OpcUa_Int32 TypeId = OpcUaId_##Name; \
  }; \
  \
  inline void Initialize(OpcUa_##Name& value) { \
    ::OpcUa_##Name##_Initialize(&value); \
  } \
  \
  inline void Clear(OpcUa_##Name& value) { \
    ::OpcUa_##Name##_Clear(&value); \
  }

OPCUA_DEFINE_MESSAGE(ActivateSessionRequest);
OPCUA_DEFINE_MESSAGE(ActivateSessionResponse);
OPCUA_DEFINE_MESSAGE(BrowseRequest);
OPCUA_DEFINE_MESSAGE(BrowseResponse);
OPCUA_DEFINE_MESSAGE(CloseSessionRequest);
OPCUA_DEFINE_MESSAGE(CloseSessionResponse);
OPCUA_DEFINE_MESSAGE(CreateSessionRequest);
OPCUA_DEFINE_MESSAGE(CreateSessionResponse);
OPCUA_DEFINE_MESSAGE(CreateSubscriptionRequest);
OPCUA_DEFINE_MESSAGE(CreateSubscriptionResponse);
OPCUA_DEFINE_MESSAGE(CreateMonitoredItemsRequest);
OPCUA_DEFINE_MESSAGE(CreateMonitoredItemsResponse);
OPCUA_DEFINE_MESSAGE(DeleteMonitoredItemsRequest);
OPCUA_DEFINE_MESSAGE(DeleteMonitoredItemsResponse);
OPCUA_DEFINE_MESSAGE(GetEndpointsRequest);
OPCUA_DEFINE_MESSAGE(GetEndpointsResponse);
OPCUA_DEFINE_MESSAGE(FindServersRequest);
OPCUA_DEFINE_MESSAGE(FindServersResponse);
OPCUA_DEFINE_MESSAGE(PublishRequest);
OPCUA_DEFINE_MESSAGE(PublishResponse);
OPCUA_DEFINE_MESSAGE(ReadRequest);
OPCUA_DEFINE_MESSAGE(ReadResponse);

} // namespace opcua
