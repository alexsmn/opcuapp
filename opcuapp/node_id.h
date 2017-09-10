#pragma once

#include "opcuapp/types.h"

inline bool operator<(const OpcUa_NodeId& a, const OpcUa_NodeId& b) {
  if (a.NamespaceIndex != b.NamespaceIndex)
    return a.NamespaceIndex < b.NamespaceIndex;

  if (a.IdentifierType != b.IdentifierType)
    return a.IdentifierType < b.IdentifierType;

  switch (a.IdentifierType) {
    case OpcUa_IdentifierType_Numeric:
      return a.Identifier.Numeric < b.Identifier.Numeric;
    case OpcUa_IdentifierType_String:
      return a.Identifier.String < b.Identifier.String;
    case OpcUa_IdentifierType_Opaque:
      return a.Identifier.ByteString < b.Identifier.ByteString;
    case OpcUa_IdentifierType_Guid:
      return *a.Identifier.Guid < *b.Identifier.Guid;
    default:
      assert(false);
      return false;
  }
}

inline bool operator==(const OpcUa_NodeId& a, opcua::NumericNodeId b) {
  return a.IdentifierType == OpcUa_IdentifierType_Numeric && a.NamespaceIndex == 0 && a.Identifier.Numeric == b;
}

namespace opcua {

OPCUA_DEFINE_METHODS(NodeId);

inline void Copy(const OpcUa_NodeId& source, OpcUa_NodeId& target) {
  target.NamespaceIndex = source.NamespaceIndex;
  target.IdentifierType = source.IdentifierType;

  switch (target.IdentifierType) {
    case OpcUa_IdentifierType_Numeric:
      target.Identifier.Numeric = source.Identifier.Numeric;
      break;
    case OpcUa_IdentifierType_String:
      Copy(source.Identifier.String, target.Identifier.String);
      break;
    case OpcUa_IdentifierType_Guid:
      assert(false);
      // Copy(source.Identifier.Guid, target.Identifier.Guid);
      break;
    case OpcUa_IdentifierType_Opaque:
      assert(false);
      // Copy(source.Identifier.ByteString, target.Identifier.ByteString);
      break;
    default:
      assert(false);
      break;
  }
}

class NodeId {
 public:
  NodeId() { Initialize(value_); }

  NodeId(NumericNodeId numeric_id, NamespaceIndex namespace_index = 0) {
    Initialize(value_);
    value_.IdentifierType = OpcUa_IdentifierType_Numeric;
    value_.Identifier.Numeric = numeric_id;
    value_.NamespaceIndex = namespace_index;
  }

  explicit NodeId(String string_id, NamespaceIndex namespace_index) {
    Initialize(value_);
    value_.IdentifierType = OpcUa_IdentifierType_String;
    value_.Identifier.String = string_id.release();
    value_.NamespaceIndex = namespace_index;
  }

  NodeId(OpcUa_NodeId&& source) : value_{source} { Initialize(source); }
  NodeId(NodeId&& source) : value_{source.value_} { Initialize(source.value_); }

  NodeId(const NodeId& source) { Copy(source.value_, value_); }
  NodeId(const OpcUa_NodeId& source) { Copy(source, value_); }

  ~NodeId() { Clear(); }

  void Clear() { opcua::Clear(value_); }

  NodeId& operator=(const NodeId& source) {
    if (&source != this) {
      Clear();
      Copy(source.value_, value_);
      Initialize(value_);
    }
    return *this;
  }

  NodeId& operator=(OpcUa_NodeId&& source) {
    if (&source != &value_) {
      Clear();
      value_ = source;
      Initialize(source);
    }
    return *this;
  }

  NodeId& operator=(NodeId&& source) {
    if (&source != this) {
      Clear();
      value_ = source.value_;
      Initialize(source.value_);
    }
    return *this;
  }

  void swap(OpcUa_NodeId& source) {
    std::swap(value_, source);
  }

  OpcUa_NodeId release() {
    auto value = value_;
    Initialize(value_);
    return value;
  }

  void CopyTo(OpcUa_NodeId& value) const {
    ::OpcUa_NodeId_Clear(&value);
    Check(::OpcUa_NodeId_CopyTo(&value_, &value));
  }

  void Reset(OpcUa_NodeId& value) {
    Clear();
    value_ = value;
    Initialize(value);
  }

  bool IsNull() const { return OpcUa_NodeId_IsNull(&const_cast<OpcUa_NodeId&>(value_)) != OpcUa_False; }

  OpcUa_NodeId& get() { return value_; }
  const OpcUa_NodeId& get() const { return value_; }

  OpcUa_IdentifierType identifier_type() const { return static_cast<OpcUa_IdentifierType>(value_.IdentifierType); }
  NamespaceIndex namespace_index() const { return value_.NamespaceIndex; }
  NumericNodeId numeric_id() const { return value_.Identifier.Numeric; }
  const OpcUa_String& string_id() const { return value_.Identifier.String; }

 private:
  OpcUa_NodeId value_;
};

inline bool operator==(const NodeId& a, OpcUa_UInt32 b) {
  return a.identifier_type() == OpcUa_IdentifierType_Numeric &&
         a.namespace_index() == 0 &&
         a.numeric_id() == b;
}

inline bool operator!=(const NodeId& a, OpcUa_UInt32 b) {
  return !(a == b);
}

inline bool operator<(const NodeId& a, const NodeId& b) {
  return a.get() < b.get();
}

} // namespace opcua
