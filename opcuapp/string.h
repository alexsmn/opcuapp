#pragma once

#include "helpers.h"

namespace opcua {

OPCUA_DEFINE_METHODS(String);

inline void Copy(const OpcUa_String& source, OpcUa_String& target) {
  Initialize(target);
  if (!OpcUa_String_IsNull(&source))
    Check(::OpcUa_String_AttachCopy(&target, OpcUa_String_GetRawString(&source)));
}

class String {
 public:
  String() { Initialize(value_); }

  String(const char* str) {
    assert(str);
    Initialize(value_);
    Check(::OpcUa_String_AttachCopy(&value_, const_cast<const OpcUa_StringA>(str)));
  }

  String(const OpcUa_StringA str) {
    assert(str);
    Initialize(value_);
    Check(::OpcUa_String_AttachCopy(&value_, str));
  }

  String(const OpcUa_String& source) { Copy(source, value_); }

  String(OpcUa_String&& source) {
    value_ = source;
    Initialize(source);
  }

  String(const String& source) {
    Copy(source.value_, value_);
  }

  String(String&& source) {
    value_ = source.value_;
    Initialize(source.value_);
  }

  ~String() { Clear(); }

  void Clear() {
    if (!is_null())
      opcua::Clear(value_);
  }

  const OpcUa_StringA raw_string() const { return OpcUa_String_GetRawString(&value_); }

  OpcUa_String* pass() const { return const_cast<OpcUa_String*>(&value_); }

  OpcUa_String release() {
    auto value = value_;
    Initialize(value_);
    return value;
  }

  String& operator=(const String& source) {
    if (&source != this) {
      Clear();
      Copy(source.value_, value_);
    }
    return *this;
  }

  bool empty() const { return ::OpcUa_String_IsEmpty(&value_) != OpcUa_False; }

  bool is_null() const { return ::OpcUa_String_IsNull(&value_) != OpcUa_False; }

  OpcUa_String& get() { return value_; }
  const OpcUa_String& get() const { return value_; }

 private:
  OpcUa_String value_;
};

inline bool operator<(const String& a, const String& b) {
  return a.get() < b.get();
}

} // namespace opcua