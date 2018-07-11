#pragma once

#include <opcuapp/basic_types.h>
#include <opcuapp/date_time.h>
#include <opcuapp/helpers.h>
#include <opcuapp/span.h>

template <class T>
inline bool operator==(T&& a, const OpcUa_Variant& b) {
  return b == a;
}

inline bool operator==(const OpcUa_Variant& a, OpcUa_Double b) {
  return a.ArrayType == OpcUa_VariantArrayType_Scalar &&
         a.Datatype == OpcUaType_Double && a.Value.Double == b;
}

namespace opcua {

class ExtensionObject;

OPCUA_DEFINE_METHODS(Variant);

void Copy(const OpcUa_Variant& source, OpcUa_Variant& target);

class Variant {
 public:
  Variant() { Initialize(value_); }
  Variant(Variant&& source) : value_{source.value_} {
    Initialize(source.value_);
  }
  Variant(OpcUa_Variant&& value) : value_{value} { Initialize(value); }
  ~Variant() { Clear(value_); }

  Variant(ExtensionObject&& extension_object);

  Variant(Byte value) {
    Initialize(value_);
    value_.Datatype = OpcUaType_Byte;
    value_.Value.Byte = value;
  }

  Variant(Int32 value) {
    Initialize(value_);
    value_.Datatype = OpcUaType_Int32;
    value_.Value.Int32 = value;
  }

  Variant(UInt32 value) {
    Initialize(value_);
    value_.Datatype = OpcUaType_UInt32;
    value_.Value.UInt32 = value;
  }

  Variant(Double value) {
    Initialize(value_);
    value_.Datatype = OpcUaType_Double;
    value_.Value.Double = value;
  }

  Variant(DateTime value) {
    Initialize(value_);
    value_.Datatype = OpcUaType_DateTime;
    value_.Value.DateTime = value.get();
  }

  Variant(const OpcUa_Variant& source) {
    Initialize(value_);
    Copy(source, value_);
  }

  Variant(const Variant& source) {
    Initialize(value_);
    Copy(source.value_, value_);
  }

  Variant& operator=(const OpcUa_Variant& source) {
    if (&source != &value_) {
      Clear(value_);
      Copy(source, value_);
    }
    return *this;
  }

  Variant& operator=(const Variant& source) {
    if (&source != this) {
      Clear(value_);
      Copy(source.value_, value_);
    }
    return *this;
  }

  Variant& operator=(OpcUa_Variant&& value) {
    if (&value != &value_) {
      Clear(value_);
      value_ = value;
      Initialize(value);
    }
    return *this;
  }

  Variant& operator=(Variant&& source) {
    if (&source != this) {
      Clear(value_);
      value_ = source.value_;
      Initialize(source.value_);
    }
    return *this;
  }

  OpcUa_Variant& get() { return value_; }
  const OpcUa_Variant& get() const { return value_; }

  BuiltInType data_type() const {
    return static_cast<BuiltInType>(value_.Datatype);
  }
  bool is_null() const { return data_type() == OpcUaType_Null; }

  VariantArrayType array_type() const { return value_.ArrayType; }
  bool is_scalar() const {
    return value_.ArrayType == OpcUa_VariantArrayType_Scalar;
  }
  bool is_array() const {
    return value_.ArrayType == OpcUa_VariantArrayType_Array;
  }
  bool is_matrix() const {
    return value_.ArrayType == OpcUa_VariantArrayType_Matrix;
  }

  template <typename T>
  T get() const;

  template <>
  Span<const OpcUa_LocalizedText> get() const {
    assert(is_array());
    auto& array = value_.Value.Array;
    return {array.Value.LocalizedTextArray, static_cast<size_t>(array.Length)};
  }

  void release(OpcUa_Variant& value) {
    value = value_;
    Initialize(value_);
  }

 private:
  OpcUa_Variant value_;
};

inline void Copy(const OpcUa_Variant& source, OpcUa_Variant& target) {
  // Copy primitive types.
  if (source.ArrayType == OpcUa_VariantArrayType_Scalar) {
    target.ArrayType = OpcUa_VariantArrayType_Scalar;
    target.Datatype = source.Datatype;
    switch (source.Datatype) {
      case OpcUaType_Null:
        return;
      case OpcUaType_Boolean:
        target.Value.Boolean = source.Value.Boolean;
        return;
      case OpcUaType_SByte:
        target.Value.SByte = source.Value.SByte;
        return;
      case OpcUaType_Byte:
        target.Value.Byte = source.Value.Byte;
        return;
      case OpcUaType_Double:
        target.Value.Double = source.Value.Double;
        return;
      case OpcUaType_DateTime:
        target.Value.DateTime = source.Value.DateTime;
        return;
      default:
        assert(false);
        return;
    }
  }
}

}  // namespace opcua

#include <opcuapp/extension_object.h>

namespace opcua {

inline Variant::Variant(ExtensionObject&& extension_object) {
  Initialize(value_);
  value_.Datatype = OpcUaType_ExtensionObject;
  ::OpcUa_ExtensionObject_Create(&value_.Value.ExtensionObject);
  extension_object.release(*value_.Value.ExtensionObject);
}

}  // namespace opcua
