#pragma once

#include <opcua.h>
#include <opcua_core.h>

namespace opcua {

using Boolean = OpcUa_Boolean;
using SByte = OpcUa_SByte;
using Byte = OpcUa_Byte;
using Int16 = OpcUa_Int16;
using UInt16 = OpcUa_UInt16;
using Int32 = OpcUa_Int32;
using UInt32 = OpcUa_UInt32;
using Double = OpcUa_Double;

using NodeClass = OpcUa_NodeClass;
using VariantArrayType = OpcUa_Byte;
using BuiltInType = OpcUa_BuiltInType;

using SubscriptionId = OpcUa_UInt32;
using AttributeId = OpcUa_UInt32;
using MonitoredItemClientHandle = OpcUa_UInt32;
using MonitoredItemId = OpcUa_UInt32;
using SequenceNumber = OpcUa_UInt32;
using NumericNodeId = OpcUa_UInt32;
using NamespaceIndex = OpcUa_UInt32;

const Boolean False = OpcUa_False;
const Boolean True = OpcUa_True;

} // namespace opcua
