#pragma once

#include <opcua.h>
#include <opcua_messagecontext.h>

namespace opcua {

#define OPCUA_DEFINE_MEMBERS_EX(Name, OpcUa_Name) \
    Name() { ::OpcUa_Name##_Initialize(this); } \
    ~Name() { ::OpcUa_Name##_Clear(this); } \
    \
    Name(const Name&) = delete; \
    Name& operator=(const Name&) = delete; \
    \
    Name(Name&& source) : OpcUa_Name{source} { \
      ::OpcUa_Name##_Initialize(&source); \
    } \
    \
    Name(OpcUa_Name&& source) : OpcUa_Name{source} { \
      ::OpcUa_Name##_Initialize(&source); \
    } \
    \
    Name& operator=(Name&& source) { \
      if (&source != this) { \
        static_cast<::OpcUa_Name&>(*this) = source; \
        ::OpcUa_Name##_Initialize(&source); \
      } \
      return *this; \
    } \
    \
    Name& operator=(OpcUa_Name&& source) { \
      if (&source != this) { \
        static_cast<::OpcUa_Name&>(*this) = source; \
        ::OpcUa_Name##_Initialize(&source); \
      } \
      return *this; \
    }

#define OPCUA_DEFINE_MEMBERS(Name) \
  OPCUA_DEFINE_MEMBERS_EX(Name, OpcUa_##Name)

#define OPCUA_DEFINE_STRUCT_EX(Name, OpcUa_Name) \
  struct Name : OpcUa_Name { \
    OPCUA_DEFINE_MEMBERS_EX(Name, OpcUa_Name) \
  \
    void release(OpcUa_Name& target) { \
      ::OpcUa_Name##_Clear(&target); \
      target = *this; \
      ::OpcUa_Name##_Initialize(this); \
    } \
  }; \
  \
  OPCUA_DEFINE_METHODS(Name)

#define OPCUA_DEFINE_STRUCT(Name) \
  OPCUA_DEFINE_STRUCT_EX(Name, OpcUa_##Name)

#define OPCUA_DEFINE_ENCODEABLE(Name) \
  struct Name : OpcUa_##Name { \
    OPCUA_DEFINE_MEMBERS(Name) \
  \
    static const OpcUa_EncodeableType& type() { return OpcUa_##Name##_EncodeableType; } \
  \
    void release(OpcUa_##Name& target) { \
      ::OpcUa_##Name##_Clear(&target); \
      target = *this; \
      ::OpcUa_##Name##_Initialize(this); \
    } \
  }; \
  \
  OPCUA_DEFINE_METHODS(Name)

OPCUA_DEFINE_STRUCT(ApplicationDescription);
OPCUA_DEFINE_STRUCT(BrowseDescription);
OPCUA_DEFINE_STRUCT(BrowseResult);
OPCUA_DEFINE_STRUCT(EndpointDescription);
OPCUA_DEFINE_STRUCT(Key);
OPCUA_DEFINE_STRUCT(MonitoredItemCreateResult);
OPCUA_DEFINE_STRUCT(MessageContext);
OPCUA_DEFINE_STRUCT(MonitoredItemNotification);
OPCUA_DEFINE_STRUCT(NotificationMessage);
OPCUA_DEFINE_STRUCT(ReadValueId);
OPCUA_DEFINE_STRUCT(ReferenceDescription);
OPCUA_DEFINE_STRUCT(RequestHeader);
OPCUA_DEFINE_STRUCT(ResponseHeader);
OPCUA_DEFINE_STRUCT(UserTokenPolicy);

OPCUA_DEFINE_ENCODEABLE(DataChangeFilter);
OPCUA_DEFINE_ENCODEABLE(DataChangeNotification);
OPCUA_DEFINE_ENCODEABLE(EventFilter);
OPCUA_DEFINE_ENCODEABLE(ServerStatusDataType);

} // namespace opcua