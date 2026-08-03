#pragma once

#include "opcua/types/extension_object.h"
#include "opcua/types/node_attributes.h"
#include "opcua/types/node_class.h"

// Bridges the flat, hand-written opcua::NodeAttributes that the node-management
// callbacks speak and the spec-conformant per-NodeClass *Attributes structures
// the AddNodes wire carries inside an ExtensionObject (OPC UA Part 4 §7.24,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.24). The generated
// ua:: codec models AddNodesItem.nodeAttributes as an ExtensionObject; these
// helpers convert to/from it at the ServiceHandler and client boundaries so the
// callback vocabulary and the SCADA bridge stay untouched.
//
// opcua::NodeAttributes can only express the attributes common to
// node-attribute bodies (display name, and for value-bearing classes the data
// type and value); the NodeClass-specific fields (executable, isAbstract, ...)
// are not modelled and round-trip as their defaults.

namespace opcua {

// Builds the ExtensionObject for the given NodeClass, filling the common
// attributes present in `attributes` and marking them in SpecifiedAttributes.
// The body is carried in the DefaultBinary encoding, the form both the binary
// and JSON (UaEncoding=1) transports accept.
ExtensionObject NodeAttributesToExtensionObject(
    NodeClass node_class,
    const NodeAttributes& attributes);

// Extracts the common attributes from a NodeAttributes ExtensionObject into the
// flat opcua::NodeAttributes. Accepts both the binary-body form (binary
// transport, or JSON with UaEncoding=1) and an inline JSON body (the form a web
// client emits). BrowseName travels on the AddNodesItem, not here.
NodeAttributes ExtensionObjectToNodeAttributes(
    NodeClass node_class,
    const ExtensionObject& node_attributes);

}  // namespace opcua
