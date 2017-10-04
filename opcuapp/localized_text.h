#pragma once

#include <opcuapp/string.h>

namespace opcua {

OPCUA_DEFINE_METHODS(LocalizedText);

class LocalizedText {
 public:
  LocalizedText() {
    Initialize(value_);
  }

  LocalizedText(OpcUa_LocalizedText&& source) : value_{source} {
    Initialize(source);
  }

  LocalizedText(LocalizedText&& source) : value_{source.value_} {
    Initialize(source.value_);
  }

  LocalizedText(const LocalizedText&) = delete;

  ~LocalizedText() {
    Clear(value_);
  }

  LocalizedText& operator=(const String& source) {
    Clear(value_.Locale);
    Copy(source.get(), value_.Text);
    return *this;
  }

  LocalizedText& operator=(const LocalizedText&) = delete;

  LocalizedText& operator=(LocalizedText&& source) {
    if (&source != this) {
      Clear(value_);
      value_ = source.value_;
      Initialize(source.value_);
    }
    return *this;
  }

  const OpcUa_String& text() const { return value_.Text; }

  bool empty() const { return ::OpcUa_String_IsEmpty(&value_.Text); }

  OpcUa_LocalizedText& get() { return value_; }
  const OpcUa_LocalizedText& get() const { return value_; }

 private:
  OpcUa_LocalizedText value_;
};

} // namespace opcua
