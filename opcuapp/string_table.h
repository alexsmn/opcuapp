#pragma once

#include "opcuapp/types.h"

namespace opcua {

class StringTable {
 public:
  StringTable() {
    ::OpcUa_StringTable_Initialize(&value_);
  }

  ~StringTable() {
    ::OpcUa_StringTable_Clear(&value_);
  }

  void Append(const String& string) {
    AddStrings(&string.get(), 1);
  }

  void AddStrings(const OpcUa_String* strings, UInt32 count) {
    ::OpcUa_StringTable_AddStrings(&value_, const_cast<OpcUa_String*>(strings), count);
  }

  void AddStringList(const OpcUa_StringA* strings) {
    ::OpcUa_StringTable_AddStringList(&value_, const_cast<OpcUa_StringA*>(strings));
  }

  String FindString(Int32 index) const {
    OpcUa_String string;
    Check(::OpcUa_StringTable_FindString(&const_cast<OpcUa_StringTable&>(value_), index, &string));
    return string;
  }

  Int32 FindIndex(const OpcUa_String& string) const {
    Int32 index;
    if (OpcUa_IsGood(::OpcUa_StringTable_FindIndex(&const_cast<OpcUa_StringTable&>(value_), &const_cast<OpcUa_String&>(string), &index)))
      return index;
    else
      return -1;
  }

  UInt32 GetCount() const { return value_.Count; }

  OpcUa_StringTable& get() { return value_; }

  const OpcUa_String& operator[](size_t index) const {
    assert(index >= 0 && index < value_.Count);
    return value_.Values[index];
  }

  const OpcUa_String* begin() const { return value_.Values; }
  const OpcUa_String* end() const { return value_.Values + value_.Count; }

 private:
  OpcUa_StringTable value_;
};

} // namespace opcua
