#pragma once

#include <opcuapp/node_id.h>

namespace opcua {

OPCUA_DEFINE_METHODS(ExpandedNodeId);

inline void Copy(const OpcUa_ExpandedNodeId& source, OpcUa_ExpandedNodeId& target) {
  Copy(source.NamespaceUri, target.NamespaceUri);
  target.ServerIndex = source.ServerIndex;
  Copy(source.NodeId, target.NodeId);
}

class ExpandedNodeId {
 public:
  ExpandedNodeId() {
    Initialize(value_);
  }

  ExpandedNodeId(NodeId node_id) {
    Initialize(value_);
    node_id.release(value_.NodeId);
  }

  ExpandedNodeId(NodeId node_id, String namespace_uri) {
    Initialize(value_);
    node_id.release(value_.NodeId);
    namespace_uri.release(value_.NamespaceUri);
  }

  ExpandedNodeId(const ExpandedNodeId& source) {
    Copy(source.value_, value_);
  }

  ExpandedNodeId(const OpcUa_ExpandedNodeId& source) {
    Copy(source, value_);
  }

  ExpandedNodeId(OpcUa_ExpandedNodeId&& source) : value_{source} {
    Initialize(source);
  }

  ~ExpandedNodeId() {
    Clear(value_);
  }

  ExpandedNodeId& operator=(const ExpandedNodeId& source) {
    if (this != &source) {
      Clear(value_);
      Copy(source.value_, value_);
    }
    return *this;
  }

  ExpandedNodeId& operator=(const OpcUa_ExpandedNodeId& source) {
    if (&value_ != &source) {
      Clear(value_);
      Copy(source, value_);
    }
    return *this;
  }

  ExpandedNodeId& operator=(ExpandedNodeId&& source) {
    if (this != &source) {
      value_ = source.value_;
      Initialize(value_);
    }
    return *this;
  }

  ExpandedNodeId& operator=(OpcUa_ExpandedNodeId&& source) {
    if (&value_ != &source) {
      value_ = source;
      Initialize(value_);
    }
    return *this;
  }

  OpcUa_ExpandedNodeId& get() { return value_; }
  const OpcUa_ExpandedNodeId& get() const { return value_; }

  void release(OpcUa_ExpandedNodeId& value) {
    value = value_;
    Initialize(value_);
  }

 private:
  OpcUa_ExpandedNodeId value_;
};

} // namespace opcua

inline bool operator==(const OpcUa_ExpandedNodeId& a, opcua::NumericNodeId b) {
  return a.ServerIndex == 0 && OpcUa_String_IsNull(&a.NamespaceUri) && a.NodeId == b;
}
