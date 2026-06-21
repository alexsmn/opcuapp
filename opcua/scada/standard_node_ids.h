#pragma once

#include "opcua/scada/node_id.h"

namespace opcua {
// Numeric identifiers of well-known Nodes in the OPC UA standard namespace
// (NamespaceIndex 0). The values are the standard NodeIds assigned by the OPC
// Foundation; the Nodes themselves belong to the standard Information Model. OPC
// UA Part 6 §A.1 NodeIds,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/A.1 (Information
// Model: OPC UA Part 5).
namespace id {

// Standard DataType NodeIds: the built-in and abstract DataTypes of the type
// hierarchy. OPC UA Part 3 §8 BuiltInTypes,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8 (NodeIds: Part 6
// §A.1).
constexpr NumericId BaseDataType = 24;
constexpr NumericId Boolean = 1;
constexpr NumericId Int8 = 2;
constexpr NumericId UInt8 = 3;
constexpr NumericId Int16 = 4;
constexpr NumericId UInt16 = 5;
constexpr NumericId Int32 = 6;
constexpr NumericId UInt32 = 7;
constexpr NumericId Int64 = 8;
constexpr NumericId UInt64 = 9;
constexpr NumericId Double = 11;
constexpr NumericId ByteString = 15;
constexpr NumericId String = 12;
constexpr NumericId QualifiedName = 20;
constexpr NumericId LocalizedText = 21;
constexpr NumericId NodeId = 17;
constexpr NumericId ExpandedNodeId = 18;
constexpr NumericId DateTime = 13;
constexpr NumericId Enumeration = 29;

// Standard ReferenceType NodeIds: the hierarchical and non-hierarchical
// reference types of the type hierarchy. OPC UA Part 3 §7 References,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/7 (NodeIds: Part 6
// §A.1).
constexpr NumericId References = 31;
constexpr NumericId NonHierarchicalReferences = 32;
constexpr NumericId HierarchicalReferences = 33;
constexpr NumericId Aggregates = 44;
constexpr NumericId HasComponent = 47;
constexpr NumericId HasProperty = 46;
constexpr NumericId HasChild = 34;
constexpr NumericId Organizes = 35;
constexpr NumericId HasTypeDefinition = 40;
constexpr NumericId HasSubtype = 45;
constexpr NumericId HasModellingRule = 37;
constexpr NumericId HasEventSource = 36;
constexpr NumericId HasNotifier = 48;

// Standard ObjectType / VariableType NodeIds: base and folder types of the type
// hierarchy. OPC UA Part 5 Information Model,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/ (NodeIds: Part 6
// §A.1).
constexpr NumericId BaseObjectType = 58;
constexpr NumericId BaseVariableType = 62;
constexpr NumericId FolderType = 61;
constexpr NumericId PropertyType = 68;

// Standard Object/Variable instance NodeIds: the root folders and the Server
// object with its ServerStatus, ServerCapabilities and OperationLimits. OPC UA
// Part 5 Information Model,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/ (NodeIds: Part 6
// §A.1).
constexpr NumericId RootFolder = 84;
constexpr NumericId ObjectsFolder = 85;
constexpr NumericId TypesFolder = 86;
constexpr NumericId Server = 2253;
constexpr NumericId Server_ServerArray = 2254;
constexpr NumericId Server_NamespaceArray = 2255;
constexpr NumericId Server_ServerStatus = 2256;
constexpr NumericId Server_ServerStatus_CurrentTime = 2258;
constexpr NumericId Server_ServerStatus_State = 2259;
constexpr NumericId Server_ServiceLevel = 2267;
constexpr NumericId Server_Auditing = 2994;
constexpr NumericId Server_ServerCapabilities = 2268;
constexpr NumericId Server_ServerCapabilities_ServerProfileArray = 2269;
constexpr NumericId Server_ServerCapabilities_LocaleIdArray = 2271;
constexpr NumericId Server_ServerCapabilities_MinSupportedSampleRate = 2272;
constexpr NumericId Server_ServerCapabilities_MaxBrowseContinuationPoints = 2735;
constexpr NumericId Server_ServerCapabilities_OperationLimits = 11704;
constexpr NumericId OperationLimits_MaxNodesPerRead = 11705;
constexpr NumericId OperationLimits_MaxNodesPerWrite = 11707;
constexpr NumericId OperationLimits_MaxNodesPerMethodCall = 11709;
constexpr NumericId OperationLimits_MaxNodesPerBrowse = 11710;
constexpr NumericId OperationLimits_MaxNodesPerRegisterNodes = 11711;
constexpr NumericId OperationLimits_MaxNodesPerTranslateBrowsePathsToNodeIds =
    11712;
constexpr NumericId OperationLimits_MaxNodesPerNodeManagement = 11713;
constexpr NumericId OperationLimits_MaxNodesPerHistoryReadData = 12165;
constexpr NumericId OperationLimits_MaxNodesPerHistoryReadEvents = 12166;
constexpr NumericId OperationLimits_MaxMonitoredItemsPerCall = 11714;

// Standard ModellingRule NodeIds: the rules that govern instance generation for
// type definitions. OPC UA Part 3 §6 Information Model concepts,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/6 (NodeIds: Part 6
// §A.1).
constexpr NumericId ModellingRules = 87;
constexpr NumericId ModellingRule_Mandatory = 78;

// Standard EventType NodeIds: base and system event types of the event model.
// OPC UA Part 5 §6 Event Model,
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6 (NodeIds: Part 6
// §A.1).
constexpr NumericId BaseEventType = 2041;
constexpr NumericId SystemEventType = 2130;
constexpr NumericId GeneralModelChangeEventType = 2133;
constexpr NumericId SemanticChangeEventType = 2738;

// Standard AggregateFunction NodeIds: the aggregate functions used by
// historical-access and aggregate reads. OPC UA Part 13 Aggregates,
// https://reference.opcfoundation.org/Core/Part13/v105/docs/ (NodeIds: Part 6
// §A.1).
constexpr NumericId AggregateFunction_Average = 2342;
constexpr NumericId AggregateFunction_Total = 2344;
constexpr NumericId AggregateFunction_Minimum = 2346;
constexpr NumericId AggregateFunction_Maximum = 2347;
constexpr NumericId AggregateFunction_Count = 2352;
constexpr NumericId AggregateFunction_Start = 2357;
constexpr NumericId AggregateFunction_End = 2358;

// Standard Alarms & Conditions Method NodeId: the Acknowledge method of the
// AcknowledgeableConditionType. OPC UA Part 9 Alarms and Conditions,
// https://reference.opcfoundation.org/Core/Part9/v105/docs/ (NodeIds: Part 6
// §A.1).
constexpr NumericId AcknowledgeableConditionType_Acknowledge = 9111;

}  // namespace id
}  // namespace opcua (vendored)
