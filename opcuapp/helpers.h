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