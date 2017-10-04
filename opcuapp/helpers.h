#pragma once

#include <opcua_builtintypes.h>

#define OpcUa_Guid_CopyTo(xSource, xDestination) OpcUa_MemCpy(xDestination, sizeof(OpcUa_Guid), xSource, sizeof(OpcUa_Guid))

#define OPCUA_DEFINE_METHODS(Name) \
  inline void Initialize(OpcUa_##Name& value) { \
    OpcUa_##Name##_Initialize(&value); \
  } \
  \
  inline void Clear(OpcUa_##Name& value) { \
    OpcUa_##Name##_Clear(&value); \
  }

#define OPCUA_DEFINE_MEMBERS_EX(Name, OpcUa_Name) \
    Name() { ::OpcUa_Name##_Initialize(this); } \
    ~Name() { ::OpcUa_Name##_Clear(this); } \
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
    Name(const Name&) = delete; \
    Name& operator=(const Name&) = delete; \
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
  \
    using OpcUa_Type = OpcUa_##Name; \
  \
    Name(const Name& source) { \
      CopyEncodeable(type(), &source, static_cast<OpcUa_Type*>(this)); \
    } \
  \
    Name& operator=(const Name& source) { \
      if (&source != this) { \
        ::OpcUa_##Name##_Clear(this); \
        CopyEncodeable(type(), &source, static_cast<OpcUa_Type*>(this)); \
      } \
      return *this; \
    } \
  \
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

/*============================================================================
* OpcUa_NodeId_CopyTo
*===========================================================================*/
inline OpcUa_StatusCode OpcUa_NodeId_CopyTo(const OpcUa_NodeId* a_pSource, OpcUa_NodeId* a_pDestination)
{
	OpcUa_InitializeStatus(OpcUa_Module_ProxyStub, "OpcUa_NodeId_CopyTo");

	OpcUa_ReturnErrorIfArgumentNull(a_pSource);
	OpcUa_ReturnErrorIfArgumentNull(a_pDestination);

	OpcUa_NodeId_Clear(a_pDestination);

	a_pDestination->IdentifierType = a_pSource->IdentifierType;
	a_pDestination->NamespaceIndex = a_pSource->NamespaceIndex;

	switch (a_pSource->IdentifierType)
	{
	case OpcUa_IdentifierType_Numeric:
	{
		a_pDestination->Identifier.Numeric = a_pSource->Identifier.Numeric;
		break;
	}
	case OpcUa_IdentifierType_String:
	{
		OpcUa_String_StrnCpy(&a_pDestination->Identifier.String, &a_pSource->Identifier.String, OPCUA_STRING_LENDONTCARE);
		break;
	}
	case OpcUa_IdentifierType_Opaque:
	{
		a_pDestination->Identifier.ByteString.Length = a_pSource->Identifier.ByteString.Length;

		if (a_pDestination->Identifier.ByteString.Length > 0)
		{
			a_pDestination->Identifier.ByteString.Data = (OpcUa_Byte*)OpcUa_Alloc(a_pSource->Identifier.ByteString.Length);
			OpcUa_GotoErrorIfAllocFailed(a_pDestination->Identifier.ByteString.Data);
			OpcUa_MemCpy(a_pDestination->Identifier.ByteString.Data, a_pDestination->Identifier.ByteString.Length, a_pSource->Identifier.ByteString.Data, a_pSource->Identifier.ByteString.Length);
		}
		else
		{
			a_pDestination->Identifier.ByteString.Data = OpcUa_Null;
		}
		break;
	}
	case OpcUa_IdentifierType_Guid:
	{
		if (a_pSource->Identifier.Guid != OpcUa_Null)
		{
			a_pDestination->Identifier.Guid = (OpcUa_Guid*)OpcUa_Alloc(sizeof(OpcUa_Guid));
			OpcUa_GotoErrorIfAllocFailed(a_pDestination->Identifier.Guid);
			OpcUa_Guid_CopyTo(a_pSource->Identifier.Guid, a_pDestination->Identifier.Guid);
		}
		else
		{
			a_pDestination->Identifier.Guid = OpcUa_Null;
		}
		break;
	}
	default:
	{
		OpcUa_GotoErrorWithStatus(OpcUa_BadInvalidArgument);
	}
	}

	OpcUa_ReturnStatusCode;
	OpcUa_BeginErrorHandling;

	OpcUa_NodeId_Clear(a_pDestination);

	OpcUa_FinishErrorHandling;
}