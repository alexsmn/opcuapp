#pragma once

#include "opcuapp/types.h"

namespace opcua {

inline void CopyEncodable(const OpcUa_EncodeableType& type, OpcUa_Void* source, OpcUa_Void*& target) {
}

template<typename T = OpcUa_Void>
class EncodeableObject {
 public:
  EncodeableObject(const OpcUa_EncodeableType& type, T* object)
      : type_{&type},
        object_{*object} {
  }

  explicit EncodeableObject(const OpcUa_EncodeableType& type)
      : type_{&type} {
    Check(::OpcUa_EncodeableObject_Create(const_cast<OpcUa_EncodeableType*>(type_), reinterpret_cast<OpcUa_Void**>(&object_)));
  }

  EncodeableObject(EncodeableObject&& source)
      : type_{source.type_},
        object_{source.object_} {
    source.object_ = nullptr;
  }

  ~EncodeableObject() {
    ::OpcUa_EncodeableObject_Delete(const_cast<OpcUa_EncodeableType*>(type_), reinterpret_cast<OpcUa_Void**>(&object_));
  }

  EncodeableObject(const EncodeableObject& source)
      : type_{source.type_} {
    Check(CopyEncodable(source.type_, source.object_, &object_));
  }

  EncodeableObject& operator=(const EncodeableObject& source) {
    if (&source != this) {
      T* object = OpcUa_Null;
      Check(CopyEncodable(*source.type_, source.object_, &object));
      object_ = object;
      type_ = source.type_;
    }
    return *this;
  }

  const OpcUa_EncodeableType& type() const { return *type_; }

  T* get() { return object_; }
  const T* get() const { return object_; }

  static EncodeableObject ParseExtension(const OpcUa_ExtensionObject& extension, const OpcUa_MessageContext& context, const OpcUa_EncodeableType& type) {
    assert(extension.Encoding == OpcUa_ExtensionObjectEncoding_Binary);
    assert(extension.Body.Binary.Length > 0);
    OpcUa_Void* object = OpcUa_Null;
    Check(::OpcUa_EncodeableObject_ParseExtension(&const_cast<OpcUa_ExtensionObject&>(extension),
        &const_cast<OpcUa_MessageContext&>(context), &const_cast<OpcUa_EncodeableType&>(type), &object));
    return EncodeableObject{type, object};
  }

  T* release() {
    auto* object = object_;
    object_ = OpcUa_Null;
    return object;
  }

  T& operator*() { return *object_; }
  const T& operator*() const { return *object_; }

 private:
  const OpcUa_EncodeableType* type_ = nullptr;
  T* object_ = OpcUa_Null;
};

} // namespace opcua
