#pragma once

#include "opcua/types.h"
#include "opcua/node_id.h"

namespace opcua {

OPCUA_DEFINE_METHODS(ExpandedNodeId);

inline void Copy(const OpcUa_ExpandedNodeId& source, OpcUa_ExpandedNodeId& target) {
  Copy(source.NamespaceUri, target.NamespaceUri);
  target.ServerIndex = source.ServerIndex;
  Copy(source.NodeId, target.NodeId);
}

class ExpandedNodeId {
 public:
  ExpandedNodeId(const OpcUa_ExpandedNodeId& source) {
    Copy(source, value_);
  }

  ExpandedNodeId(OpcUa_ExpandedNodeId&& source) : value_{source} {
    Initialize(source);
  }

  ~ExpandedNodeId() {
    Clear(value_);
  }

  OpcUa_ExpandedNodeId& get() { return value_; }
  const OpcUa_ExpandedNodeId& get() const { return value_; }

  OpcUa_ExpandedNodeId release() {
    auto value = value_;
    Initialize(value_);
    return value;
  }

 private:
  OpcUa_ExpandedNodeId value_;
};

} // namespace opcua

inline bool operator==(const OpcUa_ExpandedNodeId& a, opcua::NumericNodeId b) {
  return a.ServerIndex == 0 && OpcUa_String_IsNull(&a.NamespaceUri) && a.NodeId == b;
}
