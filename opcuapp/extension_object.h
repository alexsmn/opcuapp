#pragma once

#include <opcuapp/expanded_node_id.h>

namespace opcua {

void Copy(const OpcUa_ExtensionObject& source, OpcUa_ExtensionObject& target);

OPCUA_DEFINE_METHODS(ExtensionObject);

class ExtensionObject {
 public:
  ExtensionObject() {
    ::OpcUa_ExtensionObject_Initialize(&value_);
  }

  ExtensionObject(ExtensionObject&& source)
      : value_{source.value_} {
    ::OpcUa_ExtensionObject_Initialize(&source.value_);
  }

  ExtensionObject(OpcUa_ExtensionObject&& source)
      : value_{source} {
    ::OpcUa_ExtensionObject_Initialize(&source);
  }

  ExtensionObject(const OpcUa_EncodeableType& type, OpcUa_Void* object) {
    assert(object);
    ::OpcUa_ExtensionObject_Initialize(&value_);
    value_.Encoding = OpcUa_ExtensionObjectEncoding_EncodeableObject;
    value_.Body.EncodeableObject.Type = const_cast<OpcUa_EncodeableType*>(&type);
    value_.Body.EncodeableObject.Object = object;
    ExpandedNodeId{type.BinaryEncodingTypeId}.release(value_.TypeId);
  }

  ~ExtensionObject() {
    ::OpcUa_ExtensionObject_Clear(&value_);
  }

  ExtensionObject(const ExtensionObject& source) {
    Copy(source.value_, value_);
  }

  ExtensionObject& operator=(const ExtensionObject& source) {
    if (&source != this) {
      ::OpcUa_ExtensionObject_Clear(&value_);
      Copy(source.value_, value_);
    }
    return *this;
  }

  ExtensionObject& operator=(ExtensionObject&& source) {
    if (&source != this) {
      ::OpcUa_ExtensionObject_Clear(&value_);
      value_ = source.value_;
      ::OpcUa_ExtensionObject_Initialize(&source.value_);
    }
    return *this;
  }

  ExtensionObject& operator=(OpcUa_ExtensionObject&& source) {
    if (&source != &value_) {
      ::OpcUa_ExtensionObject_Clear(&value_);
      value_ = source;
      ::OpcUa_ExtensionObject_Initialize(&source);
    }
    return *this;
  }

  void release(OpcUa_ExtensionObject& value) {
    value = value_;
    ::OpcUa_ExtensionObject_Initialize(&value_);
  }

  const OpcUa_ExpandedNodeId& type_id() const { return value_.TypeId; }
  OpcUa_ExtensionObjectEncoding encoding() const { return value_.Encoding; }
  OpcUa_Void* object() { return value_.Body.EncodeableObject.Object; }
  OpcUa_EncodeableType* type() { return value_.Body.EncodeableObject.Type; }

  OpcUa_ExtensionObject& get() { return value_; }

  template<class T>
  static ExtensionObject Encode(T&& object) {
    OpcUa_ExtensionObject value;
    OpcUa_Void* new_object = OpcUa_Null;
    Check(::OpcUa_EncodeableObject_CreateExtension(&const_cast<OpcUa_EncodeableType&>(T::type()), &value, &new_object));
    object.release(*static_cast<typename T::OpcUa_Type*>(new_object));
    ExpandedNodeId{T::type().BinaryEncodingTypeId}.release(value.TypeId);
    return ExtensionObject{std::move(value)};
  }

 private:
  OpcUa_ExtensionObject value_;
};

} // namespace opcua

#include <opcuapp/encodable_object.h>

namespace opcua {

inline void Copy(const OpcUa_ExtensionObject& source, OpcUa_ExtensionObject& target) {
  switch (source.Encoding) {
    case OpcUa_ExtensionObjectEncoding_None:
      Initialize(target);
      break;

    case OpcUa_ExtensionObjectEncoding_Binary:
      Copy(source.Body.Binary, target.Body.Binary);
      break;

    case OpcUa_ExtensionObjectEncoding_Xml:
      assert(false);
      Initialize(target);
      // target.Body.Xml = XmlElement{source.Body.Xml}.release();
      break;

    case OpcUa_ExtensionObjectEncoding_EncodeableObject: {
      assert(source.Body.EncodeableObject.Type);
      assert(source.Body.EncodeableObject.Object);
      OpcUa_Void* object = OpcUa_Null;
      ::OpcUa_EncodeableObject_CreateExtension(source.Body.EncodeableObject.Type, &target, &object);
      CopyEncodeable(*source.Body.EncodeableObject.Type, source.Body.EncodeableObject.Object, object);
      target.Body.EncodeableObject.Type = source.Body.EncodeableObject.Type;
      target.Body.EncodeableObject.Object = object;
      break;
    }

    default:
      assert(false);
      break;
  }

  target.Encoding = source.Encoding;
  Copy(source.TypeId, target.TypeId);
  target.BodySize = source.BodySize;
}

} // namespace opcua
