#pragma once

#include <opcua.h>
#include <opcua_messagecontext.h>
#include <opcuapp/byte_string.h>
#include <opcuapp/helpers.h>
#include <opcuapp/resource.h>

namespace opcua {

OPCUA_DEFINE_METHODS(Key);

class Key {
 public:
  Key() { Initialize(value_); }

  Key(OpcUa_UInt type, ByteString key) {
    Initialize(value_);
    value_.Type = type;
    Clear(value_.Key);
    value_.Key = key.release();
  }

  Key(Key&& source) : value_{source.value_} { Initialize(source.value_); }

  ~Key() { Clear(value_); }

  OpcUa_Key& get() { return value_; }
  const OpcUa_Key& get() const { return value_; }

  Key& operator=(OpcUa_Key&& source) {
    if (&source != &value_) {
      value_ = source;
      Initialize(source);
    }
    return *this;
  }

  Key& operator=(Key&& source) { return operator=(std::move(source.value_)); }

 private:
  OpcUa_Key value_;
};

OPCUA_DEFINE_STRUCT(MessageContext);

}  // namespace opcua
