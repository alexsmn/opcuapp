#pragma once

#include <algorithm>
#include <cassert>
#include <opcua.h>
#include <opcua_binaryencoder.h>
#include <opcua_core.h>
#include <opcua_encodeableobject.h>
#include <opcua_decoder.h>
#include <opcua_memorystream.h>
#include <opcua_messagecontext.h>
#include <utility>

#include "helpers.h"
#include "span.h"
#include "status_code.h"
#include "string.h"

inline bool operator<(const OpcUa_String& a, const OpcUa_String& b) {
  return std::strcmp(OpcUa_String_GetRawString(&a), OpcUa_String_GetRawString(&b)) < 0;
}

inline bool operator<(const OpcUa_ByteString& a, const OpcUa_ByteString& b) {
  return std::lexicographical_compare(a.Data, a.Data + a.Length, b.Data, b.Data + b.Length);
}

inline bool operator<(const OpcUa_Guid& a, const OpcUa_Guid& b) {
  if (a.Data1 != b.Data1)
    return a.Data1 < b.Data1;
  if (a.Data2 != b.Data2)
    return a.Data2 < b.Data2;
  if (a.Data3 != b.Data3)
    return a.Data3 < b.Data3;
  return std::lexicographical_compare(
      std::begin(a.Data4), std::end(a.Data4),
      std::begin(b.Data4), std::end(b.Data4));
}

namespace opcua {

using Boolean = OpcUa_Boolean;
using SByte = OpcUa_SByte;
using Int16 = OpcUa_Int16;
using UInt16 = OpcUa_UInt16;
using Int32 = OpcUa_Int32;
using UInt32 = OpcUa_UInt32;
using Double = OpcUa_Double;

using NodeClass = OpcUa_NodeClass;
using VariantArrayType = OpcUa_Byte;
using BuiltInType = OpcUa_BuiltInType;

using SubscriptionId = OpcUa_UInt32;
using AttributeId = OpcUa_UInt32;
using MonitoredItemClientHandle = OpcUa_UInt32;
using MonitoredItemId = OpcUa_UInt32;
using SequenceNumber = OpcUa_UInt32;
using NumericNodeId = OpcUa_UInt32;
using NamespaceIndex = OpcUa_UInt32;

const Boolean False = OpcUa_False;
const Boolean True = OpcUa_True;

template<typename T>
class Vector {
 public:
  Vector() {}

  explicit Vector(size_t size) {
    auto* data = reinterpret_cast<T*>(::OpcUa_Memory_Alloc(sizeof(T) * size));
    if (!data)
      throw OpcUa_BadOutOfMemory;
    std::for_each(data, data + size, [](auto& v) { Initialize(v); });
    data_ = data;
    size_ = size;
  }

  Vector(Vector&& source)
      : data_{source.data_},
        size_ {source.size_} {
    source.data_ = OpcUa_Null;
    source.size_ = 0;
  }

  ~Vector() {
    std::for_each(data_, data_ + size_, [](auto& v) { Clear(v); });
    ::OpcUa_Memory_Free(data_);
  }

  Vector(const Vector&) = delete;
  Vector& operator=(const Vector&) = delete;

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }

  T* data() { return data_; }
  const T* data() const { return data_; }

  T& operator[](size_t index) { return data_[index]; }
  const T& operator[](size_t index) const { return data_[index]; }

  T* release() {
    auto* data = data_;
    data_ = nullptr;
    size_ = 0;
    return data;
  }

  T* begin() { return data_; }
  T* end() { return data_ + size_; }
  const T* begin() const { return data_; }
  const T* end() const { return data_ + size_; }

 private:
  T* data_ = OpcUa_Null;
  size_t size_ = 0;
};

OPCUA_DEFINE_METHODS(ByteString);

inline void Copy(OpcUa_ByteString source, OpcUa_ByteString& target) {
  target.Length = source.Length;
  if (target.Length >= 0) {
    target.Data = reinterpret_cast<OpcUa_Byte*>(::OpcUa_Alloc(target.Length));
    ::OpcUa_MemCpy(target.Data, target.Length, source.Data, source.Length);
  } else {
    target.Data = OpcUa_Null;
  }
}

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

inline bool operator<(const ByteString& a, const ByteString& b) {
  return a.get() < b.get();
}

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

  bool empty() const { return value_.Text.uLength == 0; }

 private:
  OpcUa_LocalizedText value_;
};

OPCUA_DEFINE_METHODS(QualifiedName);

inline void Copy(const OpcUa_QualifiedName& source, OpcUa_QualifiedName& target) {
  target.NamespaceIndex = source.NamespaceIndex;
  Copy(source.Name, target.Name);
}

class QualifiedName {
 public:
  QualifiedName() {
    Initialize(value_);
  }

  QualifiedName(OpcUa_QualifiedName&& source) : value_{source} {
    Initialize(source);
  }

  QualifiedName(QualifiedName&& source) : value_{source.value_} {
    Initialize(source.value_);
  }

  QualifiedName(const QualifiedName& source) = delete;

  ~QualifiedName() {
    Clear(value_);
  }

  QualifiedName& operator=(const QualifiedName& source) {
    if (&source != this)
      Copy(source.value_, value_);
    return *this;
  }

  QualifiedName& operator=(QualifiedName&& source) {
    if (&source != this) {
      Clear(value_);
      value_ = source.value_;
      Initialize(source.value_);
    }
    return *this;
  }

  bool empty() const { return value_.Name.uLength == 0; }
  const OpcUa_String& name() const { return value_.Name; }
  NamespaceIndex namespace_index() const { return value_.NamespaceIndex; }

 private:
  OpcUa_QualifiedName value_;
};

class DateTime {
 public:
  DateTime() { ::OpcUa_DateTime_Initialize(&value_); }

  OpcUa_DateTime get() const { return value_; }
  UInt16 picoseconds() const { return picoseconds_; }

  static DateTime UtcNow() { return DateTime{::OpcUa_DateTime_UtcNow()}; }

 private:
  explicit DateTime(OpcUa_DateTime value) : value_{value} {}

  OpcUa_DateTime value_;
  UInt16 picoseconds_ = 0;
};

} // namespace opcua
