#pragma once

#include <cassert>
#include <opcua.h>
#include <opcua_encodeableobject.h>
#include <opcua_messagecontext.h>
#include <opcuapp/status_code.h>
#include <opcuapp/expanded_node_id.h>

namespace opcua {

void CopyEncodable(const OpcUa_EncodeableType& type, const OpcUa_Void* source, OpcUa_Void* target);

class EncodeableObject {
 public:
  EncodeableObject() {}

  EncodeableObject(const OpcUa_EncodeableType& type, OpcUa_Void* object)
      : type_{&type},
        object_{object} {
  }

  explicit EncodeableObject(const OpcUa_EncodeableType& type)
      : type_{&type} {
    Check(::OpcUa_EncodeableObject_Create(const_cast<OpcUa_EncodeableType*>(type_), &object_));
  }

  EncodeableObject(EncodeableObject&& source)
      : type_{source.type_},
        object_{source.object_} {
    source.type_ = OpcUa_Null;
    source.object_ = OpcUa_Null;
  }

  EncodeableObject(const EncodeableObject& source)
      : type_{source.type_} {
    if (source.object_) {
      Check(::OpcUa_EncodeableObject_Create(const_cast<OpcUa_EncodeableType*>(type_), &object_));
      CopyEncodable(*type_, source.object_, object_);
    }
  }

  ~EncodeableObject() { Clear(); }

  EncodeableObject& operator=(const EncodeableObject& source) {
    if (&source != this) {
      Clear();
      OpcUa_Void* object = OpcUa_Null;
      if (source.object_) {
        assert(source.type_);
        Check(::OpcUa_EncodeableObject_Create(const_cast<OpcUa_EncodeableType*>(type_), &object));
        CopyEncodable(*type_, source.object_, object);
      }
      object_ = object;
      type_ = source.type_;
    }
    return *this;
  }

  const OpcUa_EncodeableType* type() const { return type_; }

  ExpandedNodeId type_id() const {
    return type_ ? ExpandedNodeId{type_->BinaryEncodingTypeId, type_->NamespaceUri} : ExpandedNodeId{};
  }

  OpcUa_Void* get() { return object_; }
  const OpcUa_Void* get() const { return object_; }

  void Clear() {
    if (object_) {
      ::OpcUa_EncodeableObject_Delete(const_cast<OpcUa_EncodeableType*>(type_), reinterpret_cast<OpcUa_Void**>(&object_));
      type_ = OpcUa_Null;
      object_ = OpcUa_Null;
    }
  }

  OpcUa_Void* release() {
    auto* object = object_;
    object_ = OpcUa_Null;
    type_ = OpcUa_Null;
    return object;
  }

  explicit operator bool() const { return !!object_; }

  static EncodeableObject ParseExtension(const OpcUa_ExtensionObject& extension, const OpcUa_MessageContext& context, const OpcUa_EncodeableType& type) {
    assert(extension.Encoding == OpcUa_ExtensionObjectEncoding_Binary);
    assert(extension.Body.Binary.Length > 0);
    OpcUa_Void* object = OpcUa_Null;
    Check(::OpcUa_EncodeableObject_ParseExtension(&const_cast<OpcUa_ExtensionObject&>(extension),
        &const_cast<OpcUa_MessageContext&>(context), &const_cast<OpcUa_EncodeableType&>(type), &object));
    return EncodeableObject{type, object};
  }

 private:
  const OpcUa_EncodeableType* type_ = OpcUa_Null;
  OpcUa_Void* object_ = OpcUa_Null;
};

template<class T>
inline EncodeableObject Encode(T&& source) {
  EncodeableObject encodeable{typename T::type()};
  source.release(*static_cast<T*>(encodeable.get()));
  return encodeable;
}

} // namespace opcua
