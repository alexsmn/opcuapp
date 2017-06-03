#pragma once

#include <opcua.h>
#include <opcua_encodeableobject.h>
#include <opcua_messagecontext.h>

#include "opcuapp/encodable_object.h"
#include "opcuapp/types.h"

namespace opcua {

#define OPCUA_DEFINE_MEMBERS(Name) \
    Name() { ::OpcUa_##Name##_Initialize(this); } \
    ~Name() { ::OpcUa_##Name##_Clear(this); } \
    \
    Name(const Name&) = delete; \
    Name& operator=(const Name&) = delete; \
    \
    Name(Name&& source) : OpcUa_##Name{source} { \
      ::OpcUa_##Name##_Initialize(&source); \
    } \
    \
    Name(OpcUa_##Name&& source) : OpcUa_##Name{source} { \
      ::OpcUa_##Name##_Initialize(&source); \
    } \
    \
    Name& operator=(Name&& source) { \
      if (&source != this) { \
        static_cast<::OpcUa_##Name&>(*this) = source; \
        ::OpcUa_##Name##_Initialize(&source); \
      } \
      return *this; \
    } \
    \
    Name& operator=(OpcUa_##Name&& source) { \
      if (&source != this) { \
        static_cast<::OpcUa_##Name&>(*this) = source; \
        ::OpcUa_##Name##_Initialize(&source); \
      } \
      return *this; \
    }

#define OPCUA_DEFINE_STRUCT(Name) \
  struct Name : OpcUa_##Name { \
    OPCUA_DEFINE_MEMBERS(Name) \
  \
    void release(OpcUa_##Name& target) { \
      ::OpcUa_##Name##_Clear(&target); \
      target = *this; \
      ::OpcUa_##Name##_Initialize(this); \
    } \
  }; \
  \
  OPCUA_DEFINE_METHODS(Name)

#define OPCUA_DEFINE_ENCODEABLE(Name) \
  struct Name : OpcUa_##Name { \
    OPCUA_DEFINE_MEMBERS(Name) \
  \
    static const OpcUa_EncodeableType& type() { return OpcUa_##Name##_EncodeableType; } \
  \
    EncodeableObject<OpcUa_##Name> Encode() { \
      EncodeableObject<OpcUa_##Name> encodeable{type()}; \
      *static_cast<OpcUa_##Name*>(encodeable.get()) = *this; \
      ::OpcUa_##Name##_Initialize(this); \
      return encodeable; \
    } \
  }

OPCUA_DEFINE_STRUCT(ApplicationDescription);
OPCUA_DEFINE_STRUCT(BrowseDescription);
OPCUA_DEFINE_STRUCT(BrowseResult);
OPCUA_DEFINE_STRUCT(DataChangeNotification);
OPCUA_DEFINE_STRUCT(DataValue);
OPCUA_DEFINE_STRUCT(EndpointDescription);
OPCUA_DEFINE_STRUCT(MessageContext);
OPCUA_DEFINE_STRUCT(MonitoredItemCreateResult);
OPCUA_DEFINE_STRUCT(MonitoredItemNotification);
OPCUA_DEFINE_STRUCT(NotificationMessage);
OPCUA_DEFINE_STRUCT(ReadValueId);
OPCUA_DEFINE_STRUCT(ReferenceDescription);
OPCUA_DEFINE_STRUCT(RequestHeader);
OPCUA_DEFINE_STRUCT(ResponseHeader);
OPCUA_DEFINE_STRUCT(UserTokenPolicy);

OPCUA_DEFINE_ENCODEABLE(DataChangeFilter);
OPCUA_DEFINE_ENCODEABLE(EventFilter);

} // namespace opcua