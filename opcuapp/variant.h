#pragma once

#include "opcuapp/types.h"

namespace opcua {

OPCUA_DEFINE_METHODS(Variant);

class Variant {
 public:
  Variant() { Initialize(value_); }
  Variant(const Variant&) = delete;
  Variant(Variant&& source) : value_{source.value_} { Initialize(source.value_); }
  Variant(OpcUa_Variant&& value) : value_{value} { Initialize(value); }
  ~Variant() { Clear(value_); }

  Variant& operator=(const Variant&) = delete;

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

  BuiltInType data_type() const { return static_cast<BuiltInType>(value_.Datatype); }
  bool is_null() const { return data_type() == OpcUaType_Null; }

  VariantArrayType array_type() const { return value_.ArrayType; }
  bool is_scalar() const { return value_.ArrayType == OpcUa_VariantArrayType_Scalar; }
  bool is_array() const { return value_.ArrayType == OpcUa_VariantArrayType_Array; }
  bool is_matrix() const { return value_.ArrayType == OpcUa_VariantArrayType_Matrix; }

  template<typename T> T get() const;

  template<>
  inline opcua::Span<const OpcUa_LocalizedText> get() const {
    assert(is_array());
    auto& array = value_.Value.Array;
    return {array.Value.LocalizedTextArray, static_cast<size_t>(array.Length)};
  }

 private:
  OpcUa_Variant value_;
};

} // namespace opcua
