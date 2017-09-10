#pragma once

#include <opcuapp/expanded_node_id.h>
#include <opcuapp/encodable_object.h>

namespace opcua {

inline void Copy(const OpcUa_ExtensionObject& source, OpcUa_ExtensionObject& target) {
  target.Encoding = source.Encoding;
  Copy(source.TypeId, target.TypeId);
  target.BodySize = source.BodySize;
  switch (source.Encoding) {
    case OpcUa_ExtensionObjectEncoding_None:
      break;
    case OpcUa_ExtensionObjectEncoding_Binary:
      Copy(source.Body.Binary, target.Body.Binary);
      break;
    case OpcUa_ExtensionObjectEncoding_Xml:
      assert(false);
      // target.Body.Xml = XmlElement{source.Body.Xml}.release();
      break;
    case OpcUa_ExtensionObjectEncoding_EncodeableObject: {
      assert(source.Body.EncodeableObject.Type);
      assert(source.Body.EncodeableObject.Object);
      EncodeableObject object{*source.Body.EncodeableObject.Type};
      CopyEncodable(*source.Body.EncodeableObject.Type, source.Body.EncodeableObject.Object, object.get());
      target.Body.EncodeableObject.Type = source.Body.EncodeableObject.Type;
      target.Body.EncodeableObject.Object = object.release();
      break;
    }
    default:
      assert(false);
      break;
  }
}

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

  explicit ExtensionObject(EncodeableObject&& encodeable) {
    ::OpcUa_ExtensionObject_Initialize(&value_);
    if (encodeable) {
      encodeable.type_id().release(value_.TypeId);
      value_.Encoding = OpcUa_ExtensionObjectEncoding_EncodeableObject;
      value_.Body.EncodeableObject.Type = const_cast<OpcUa_EncodeableType*>(encodeable.type());
      value_.Body.EncodeableObject.Object = encodeable.release();
    }
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

  const OpcUa_ExpandedNodeId& type_id() const { return value_.TypeId; }
  OpcUa_ExtensionObjectEncoding encoding() const { return value_.Encoding; }
  OpcUa_Void* object() { return value_.Body.EncodeableObject.Object; }
  OpcUa_EncodeableType* type() { return value_.Body.EncodeableObject.Type; }

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

  ExtensionObject& operator=(EncodeableObject&& source) {
    ::OpcUa_ExtensionObject_Clear(&value_);
    if (source) {
      source.type_id().release(value_.TypeId);
      value_.Encoding = OpcUa_ExtensionObjectEncoding_EncodeableObject;
      value_.Body.EncodeableObject.Type = const_cast<OpcUa_EncodeableType*>(source.type());
      value_.Body.EncodeableObject.Object = source.release();
    }
    return *this;
  }

  void swap(OpcUa_ExtensionObject& value) {
    std::swap(value_, value);
  }

  void Release(OpcUa_ExtensionObject& value) {
    ::OpcUa_ExtensionObject_Clear(&value);
    value = value_;
    ::OpcUa_ExtensionObject_Initialize(&value_);
  }

  OpcUa_ExtensionObject& get() { return value_; }

 private:
  OpcUa_ExtensionObject value_;
};

} // namespace opcua
