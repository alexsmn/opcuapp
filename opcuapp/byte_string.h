#pragma once

#include <algorithm>
#include <opcua.h>
#include <opcuapp/helpers.h>

namespace opcua {

inline void Copy(const OpcUa_ByteString& source, OpcUa_ByteString& target) {
  target.Length = source.Length;
  if (target.Length >= 0) {
    target.Data = reinterpret_cast<OpcUa_Byte*>(::OpcUa_Alloc(target.Length));
    ::OpcUa_MemCpy(target.Data, target.Length, source.Data, source.Length);
  } else {
    target.Data = OpcUa_Null;
  }
}

OPCUA_DEFINE_METHODS(ByteString);

class ByteString {
 public:
  ByteString() { Initialize(value_); }
  ByteString(OpcUa_ByteString&& source) : value_{source} { Initialize(source); }
  ByteString(ByteString&& source) : value_{source.value_} { Initialize(source.value_); }
  ByteString(const ByteString&) = delete;

  ByteString(const void* data, size_t size) {
    value_.Data = reinterpret_cast<OpcUa_Byte*>(OpcUa_Alloc(size));
    memcpy(value_.Data, data, size);
    value_.Length = size;
  }

  ~ByteString() { Clear(); }

  void Clear() { opcua::Clear(value_); }

  ByteString& operator=(const ByteString&) = delete;

  ByteString& operator=(OpcUa_ByteString&& source) {
    MoveAssign(std::move(source));
    return *this;
  }

  ByteString& operator=(ByteString&& source) {
    MoveAssign(std::move(source.value_));
    return *this;
  }

  void swap(OpcUa_ByteString& source) {
    if (&value_ != &source)
      std::swap(value_, source);
  }

  const OpcUa_ByteString& get() const { return value_; }

  OpcUa_ByteString release() {
    auto value = value_;
    Initialize(value_);
    return value;
  }

  const OpcUa_Int32 size() const { return value_.Length; }
  const OpcUa_Byte* data() const { return value_.Data; }

 private:
  void MoveAssign(OpcUa_ByteString&& source) {
    if (&value_ != &source) {
      Clear();
      value_ = source;
      Initialize(source);
    }
  }

  OpcUa_ByteString value_;
};

} // namespace opcua

inline bool operator<(const OpcUa_ByteString& a, const OpcUa_ByteString& b) {
  return std::lexicographical_compare(a.Data, a.Data + a.Length, b.Data, b.Data + b.Length);
}

inline bool operator<(const opcua::ByteString& a, const opcua::ByteString& b) {
  return a.get() < b.get();
}
